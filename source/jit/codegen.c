/*
 * jit/codegen.c -- IR -> x86_64 native code emitter (Phase 6 v1).
 *
 * Calling convention (System V AMD64):
 *   arg slots:  RDI=vm  RSI=t  DL=skip_invariant
 *               RCX=frame_slots  R8=vals
 *   return:     EAX = CandoTraceStatus
 *
 * Register usage inside the emitted body:
 *   RBX = vm                      (saved from RDI on entry)
 *   R12 = t                        (saved from RSI on entry)
 *   R15 = frame_slots              (saved from RCX on entry)
 *   R14 = vals                     (saved from R8  on entry)
 *   XMM0/XMM1 = scratch per op
 *
 * RBX/R12/R14/R15 are callee-saved per the SysV ABI; we push/pop
 * them in the prologue/epilogue.  XMM0/XMM1 are caller-saved which
 * means side-exit C calls don't have to preserve them (we don't
 * carry XMM state across calls).
 *
 * Layout of the emitted function:
 *   prologue
 *   body              (one chunk per IR op, in linear order)
 *   loop_done:        epilogue returning TRACE_LOOP_DONE (= 0)
 *   per-guard stub_K  (loads snap_idx into r9d, jumps to common)
 *   ...
 *   side_exit_common: argc-marshal, call replay helper,
 *                     return TRACE_GUARD_FAILED
 *
 * The IR-interp's behaviour is the contract: we emit one trace iter,
 * return LOOP_DONE on clean close, GUARD_FAILED on guard miss.
 * v1 ignores `skip_invariant` (LICM); every IR op runs every iter.
 * The IR ops we don't yet handle abort codegen and the trace falls
 * back to the IR-interpreter unchanged.
 *
 * Linux x86_64 only.  Windows/macOS port lives behind a future
 * ifdef when Phase 8 portability lands.
 */

#include "codegen.h"
#include "../vm/vm.h"

#include <string.h>
#include <stddef.h>

/* External symbol for the side-exit helper (exported from jit.c). */
void cando_jit_replay_snapshot_for_mcode(struct CandoVM *vm,
                                          CandoTrace *t,
                                          TraceVal *vals,
                                          CandoValue *frame_slots,
                                          u32 snap_idx);

#define CG_MAX_GUARDS  128
#define CG_BUF_SIZE    4096u    /* one page; bail if a trace needs more */

typedef struct {
    u8  *base;          /* start of mcode buffer */
    u8  *cur;           /* current write pointer */
    u8  *end;           /* one past the last writable byte */
    bool failed;        /* set on overflow or unsupported op */
    struct CandoVM *vm; /* threaded through for fast-native lookup */
    u16 cur_snap;       /* most-recent guard's snapshot index, used by
                           IR_GLOAD/IR_GSTORE bad-type side-exits to
                           match the IR-interpreter's behaviour */

    /* Patch table for per-guard side-exit jumps.  Each entry is the
     * offset (relative to base) of a 4-byte placeholder displacement
     * inside a JE/JNE instruction.  After the body is emitted we know
     * each guard's stub address and patch the displacement. */
    struct {
        u32 je_disp_off;   /* offset of the 4-byte disp32 inside je    */
        u16 snap_idx;      /* snap idx to load into the stub           */
        u32 stub_off;      /* filled in when the stub is emitted later */
    } guards[CG_MAX_GUARDS];
    u32 guard_count;
} CG;

/* ============================================================ */
/* Byte-level helpers                                            */
/* ============================================================ */

static void cg_emit_bytes(CG *cg, const u8 *p, u32 n) {
    if (cg->failed) return;
    if (cg->cur + n > cg->end) { cg->failed = true; return; }
    memcpy(cg->cur, p, n);
    cg->cur += n;
}

static void cg_emit_u8(CG *cg, u8 b) {
    cg_emit_bytes(cg, &b, 1);
}

static void cg_emit_u32(CG *cg, u32 v) {
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    cg_emit_bytes(cg, b, 4);
}

static void cg_emit_u64(CG *cg, u64 v) {
    cg_emit_u32(cg, (u32)v);
    cg_emit_u32(cg, (u32)(v >> 32));
}

static u32 cg_off(const CG *cg) { return (u32)(cg->cur - cg->base); }

static void cg_patch_u32(CG *cg, u32 off, u32 v) {
    if (off + 4 > cg->end - cg->base) return;
    u8 *p = cg->base + off;
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* ============================================================ */
/* x86_64 instruction encodings                                  */
/* ============================================================ */

/* mov rax, [r14 + 8*idx]    -- load 8 bytes from vals[idx] into rax. */
static void emit_mov_rax_vals(CG *cg, u32 idx) {
    /* REX.W + REX.B + opcode 8B + ModR/M(10 000 110)=0x86 + disp32 */
    static const u8 prefix[] = { 0x49, 0x8B, 0x86 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* mov [r14 + 8*idx], rax    -- store rax into vals[idx]. */
static void emit_mov_vals_rax(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x89, 0x86 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}

/* movsd xmmN, [r14 + 8*idx]  for N in 0..1 (we only use 0/1). */
static void emit_movsd_xmm_vals(CG *cg, u8 xmm, u32 idx) {
    /* F2 41 0F 10 /r [disp32], REX.B=1 (base R14).  ModR/M:
     *   mod=10 (disp32), reg=xmm, r/m=110 (R14&7=6)
     *   r/m for SIB-less [r14+disp32] is 110.  ModR/M = 0x86 | (xmm<<3) */
    cg_emit_u8(cg, 0xF2);
    cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x10);
    cg_emit_u8(cg, (u8)(0x86 | (xmm << 3)));
    cg_emit_u32(cg, idx * 8);
}
/* movsd [r14 + 8*idx], xmmN. */
static void emit_movsd_vals_xmm(CG *cg, u32 idx, u8 xmm) {
    cg_emit_u8(cg, 0xF2);
    cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x11);
    cg_emit_u8(cg, (u8)(0x86 | (xmm << 3)));
    cg_emit_u32(cg, idx * 8);
}

/* mov rax, [r15 + 8*slot]   -- load 8 bytes from frame_slots[slot]. */
static void emit_mov_rax_slot(CG *cg, u32 slot) {
    /* REX.W + REX.B + 8B /r, base R15 -> r/m=111 */
    static const u8 prefix[] = { 0x49, 0x8B, 0x87 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, slot * 8);
}
/* mov [r15 + 8*slot], rax. */
static void emit_mov_slot_rax(CG *cg, u32 slot) {
    static const u8 prefix[] = { 0x49, 0x89, 0x87 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, slot * 8);
}

/* movabs rax, imm64          -- load 64-bit immediate. */
static void emit_movabs_rax(CG *cg, u64 imm) {
    cg_emit_u8(cg, 0x48);
    cg_emit_u8(cg, 0xB8);
    cg_emit_u64(cg, imm);
}
/* movabs rcx, imm64. */
static void emit_movabs_rcx(CG *cg, u64 imm) {
    cg_emit_u8(cg, 0x48);
    cg_emit_u8(cg, 0xB9);
    cg_emit_u64(cg, imm);
}
/* movabs rsi, imm64          -- 48 BE imm64. */
static void emit_movabs_rsi(CG *cg, u64 imm) {
    cg_emit_u8(cg, 0x48);
    cg_emit_u8(cg, 0xBE);
    cg_emit_u64(cg, imm);
}
/* lea rdx, [r14 + 8*idx]     -- compute address of vals[idx].
 * REX = 0x49 (W=1 R=0 X=0 B=1 -- B for r14 base).
 * Opcode 8D /r.  ModR/M = 10 010 110 = 0x96 (mod=disp32, reg=rdx,
 * r/m=110 since R14&7=110 needs no SIB). */
static void emit_lea_rdx_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x8D, 0x96 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* test eax, eax              -- 85 C0.  Sets ZF=1 iff eax==0. */
static void emit_test_eax_eax(CG *cg) {
    static const u8 b[] = { 0x85, 0xC0 };
    cg_emit_bytes(cg, b, 2);
}

/* and rax, rcx               -- 48 21 c8. */
static void emit_and_rax_rcx(CG *cg) {
    static const u8 b[] = { 0x48, 0x21, 0xC8 };
    cg_emit_bytes(cg, b, 3);
}
/* cmp rax, rcx               -- 48 39 c8. */
static void emit_cmp_rax_rcx(CG *cg) {
    static const u8 b[] = { 0x48, 0x39, 0xC8 };
    cg_emit_bytes(cg, b, 3);
}
/* mov rdx, rax               -- 48 89 c2 (rax -> rdx). */
static void emit_mov_rdx_rax(CG *cg) {
    static const u8 b[] = { 0x48, 0x89, 0xC2 };
    cg_emit_bytes(cg, b, 3);
}

/* je rel32 with 4-byte displacement; returns offset of the disp32
 * (so it can be patched later).  Encoding: 0F 84 disp32. */
static u32 emit_je_rel32_placeholder(CG *cg) {
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x84);
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    return disp_off;
}

/* jmp rel32 -- E9 disp32.  Returns offset of the disp32. */
static u32 emit_jmp_rel32_placeholder(CG *cg) {
    cg_emit_u8(cg, 0xE9);
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    return disp_off;
}

/* Patch a placeholder rel32 at `disp_off` so it targets `target`. */
static void cg_patch_rel32(CG *cg, u32 disp_off, u32 target) {
    /* rel32 is `target - (disp_off + 4)` */
    u32 rel = target - (disp_off + 4);
    cg_patch_u32(cg, disp_off, rel);
}

/* xorpd xmm1, xmm1           -- 66 0F 57 C9. */
static void emit_xorpd_xmm1_xmm1(CG *cg) {
    static const u8 b[] = { 0x66, 0x0F, 0x57, 0xC9 };
    cg_emit_bytes(cg, b, 4);
}

/* ucomisd xmm0, xmm1         -- 66 0F 2E C1. */
static void emit_ucomisd_xmm0_xmm1(CG *cg) {
    static const u8 b[] = { 0x66, 0x0F, 0x2E, 0xC1 };
    cg_emit_bytes(cg, b, 4);
}
/* addsd xmm0, xmm1   -- F2 0F 58 C1. */
static void emit_addsd(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x58, 0xC1 };
    cg_emit_bytes(cg, b, 4);
}
/* subsd xmm0, xmm1   -- F2 0F 5C C1. */
static void emit_subsd(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x5C, 0xC1 };
    cg_emit_bytes(cg, b, 4);
}
/* mulsd xmm0, xmm1   -- F2 0F 59 C1. */
static void emit_mulsd(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x59, 0xC1 };
    cg_emit_bytes(cg, b, 4);
}
/* divsd xmm0, xmm1   -- F2 0F 5E C1. */
static void emit_divsd(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x5E, 0xC1 };
    cg_emit_bytes(cg, b, 4);
}
/* subsd xmm1, xmm0   -- F2 0F 5C C8.  Used to compute -x via 0 - x:
 * caller zeroes xmm1 with xorpd, loads x into xmm0, then this
 * leaves -x in xmm1. */
static void emit_subsd_xmm1_xmm0(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x5C, 0xC8 };
    cg_emit_bytes(cg, b, 4);
}

/* setb al            -- 0F 92 C0 (CF=1).  For LT after ucomisd. */
static void emit_setb_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x92, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* sete al            -- 0F 94 C0 (ZF=1). */
static void emit_sete_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x94, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* setne al           -- 0F 95 C0 (ZF=0). */
static void emit_setne_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x95, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* setbe al           -- 0F 96 C0 (CF=1 || ZF=1).  For LE. */
static void emit_setbe_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x96, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* seta al            -- 0F 97 C0 (CF=0 && ZF=0).  For GT. */
static void emit_seta_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x97, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* setae al           -- 0F 93 C0 (CF=0).  For GE. */
static void emit_setae_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0x93, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* movzx eax, al      -- 0F B6 C0. */
static void emit_movzx_eax_al(CG *cg) {
    static const u8 b[] = { 0x0F, 0xB6, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* cvtsi2sd xmm0, eax -- F2 0F 2A C0. */
static void emit_cvtsi2sd_xmm0_eax(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x2A, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}

/* xor eax, eax       -- 31 C0. */
static void emit_xor_eax_eax(CG *cg) {
    static const u8 b[] = { 0x31, 0xC0 };
    cg_emit_bytes(cg, b, 2);
}
/* mov eax, imm32     -- B8 imm32. */
static void emit_mov_eax_imm(CG *cg, u32 imm) {
    cg_emit_u8(cg, 0xB8);
    cg_emit_u32(cg, imm);
}
/* mov r9d, imm32     -- 41 B9 imm32. */
static void emit_mov_r9d_imm(CG *cg, u32 imm) {
    cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0xB9);
    cg_emit_u32(cg, imm);
}

/* Reg-to-reg moves used in the side-exit common stub.
 *   mov rdi, rbx   ; 48 89 DF
 *   mov rsi, r12   ; 4C 89 E6
 *   mov rdx, r14   ; 4C 89 F2
 *   mov rcx, r15   ; 4C 89 FF
 *   mov r8, r9     ; 4D 89 C8     (r9 carries the snap idx)
 */
static void emit_mov_rdi_rbx(CG *cg) {
    static const u8 b[] = { 0x48, 0x89, 0xDF }; cg_emit_bytes(cg, b, 3);
}
static void emit_mov_rsi_r12(CG *cg) {
    static const u8 b[] = { 0x4C, 0x89, 0xE6 }; cg_emit_bytes(cg, b, 3);
}
static void emit_mov_rdx_r14(CG *cg) {
    static const u8 b[] = { 0x4C, 0x89, 0xF2 }; cg_emit_bytes(cg, b, 3);
}
static void emit_mov_rcx_r15(CG *cg) {
    /* ModR/M = 11 111 001: reg=r15&7=111, r/m=rcx=001.  An earlier
     * draft had 0xFF (encodes mov rdi, r15) which silently clobbered
     * vm at the side-exit, sending garbage into the snapshot helper. */
    static const u8 b[] = { 0x4C, 0x89, 0xF9 }; cg_emit_bytes(cg, b, 3);
}
static void emit_mov_r8_r9(CG *cg) {
    static const u8 b[] = { 0x4D, 0x89, 0xC8 }; cg_emit_bytes(cg, b, 3);
}

/* call rax           -- FF D0. */
static void emit_call_rax(CG *cg) {
    static const u8 b[] = { 0xFF, 0xD0 }; cg_emit_bytes(cg, b, 2);
}

/* ============================================================ */
/* High-level emitters                                           */
/* ============================================================ */

/* Prologue:
 *   push rbp              ; 55
 *   mov  rbp, rsp         ; 48 89 e5
 *   push rbx              ; 53
 *   push r12              ; 41 54
 *   push r14              ; 41 56
 *   push r15              ; 41 57
 *   mov  rbx, rdi         ; 48 89 fb            -- vm
 *   mov  r12, rsi         ; 49 89 f4            -- t
 *   mov  r15, rcx         ; 49 89 cf            -- frame_slots
 *   mov  r14, r8          ; 4d 89 c6            -- vals
 *
 * Stack discipline: caller aligns RSP to 16 before `call mcode_fn`,
 * which pushes the return address (RSP -> aligned-8).  Five pushes
 * (rbp, rbx, r12, r14, r15) bring us back to aligned (5*8=40, plus
 * the 8 from the return address = 48 bytes from caller_rsp).  We
 * deliberately don't sub rsp by 8 here: if we did, the side-exit's
 * `call rax` to the snapshot helper would land on a misaligned
 * stack and the helper's internal SSE ops could fault.
 */
static void emit_prologue(CG *cg) {
    static const u8 b[] = {
        0x55,
        0x48, 0x89, 0xE5,
        0x53,
        0x41, 0x54,
        0x41, 0x56,
        0x41, 0x57,
        0x48, 0x89, 0xFB,
        0x49, 0x89, 0xF4,
        0x49, 0x89, 0xCF,
        0x4D, 0x89, 0xC6,
    };
    cg_emit_bytes(cg, b, sizeof(b));
}

/* Epilogue restoring callee-saveds and returning.  Caller has set EAX. */
static void emit_epilogue(CG *cg) {
    static const u8 b[] = {
        0x41, 0x5F,                /* pop r15        */
        0x41, 0x5E,                /* pop r14        */
        0x41, 0x5C,                /* pop r12        */
        0x5B,                      /* pop rbx        */
        0x5D,                      /* pop rbp        */
        0xC3,                      /* ret            */
    };
    cg_emit_bytes(cg, b, sizeof(b));
}

/* IR_SLOAD slot: load frame_slots[slot] into rax, type-check that
 * (rax & NB_MASK) != NB_MASK, side-exit (no snapshot rollback --
 * SLOAD bad-type returns TRACE_BAD_TYPE in IR-interp; for v1 we
 * treat it as TRACE_GUARD_FAILED with snap=0).  Then store the
 * 8-byte u64 to vals[i] (same bit pattern; works because cando_is_
 * number numbers are stored as raw f64 bits and that's what vals[]
 * .d wants too -- the IR-interp treats vals[i].d as f64 directly
 * via the union). */
static void emit_sload(CG *cg, u32 slot, u32 i) {
    emit_mov_rax_slot(cg, slot);
    emit_movabs_rcx(cg, 0xFFF8000000000000ULL);   /* NB_MASK */
    emit_mov_rdx_rax(cg);
    emit_and_rax_rcx(cg);                         /* rax = v.u & MASK */
    emit_cmp_rax_rcx(cg);
    /* je side_exit (boxed value) */
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg->guards[cg->guard_count].je_disp_off = emit_je_rel32_placeholder(cg);
    cg->guards[cg->guard_count].snap_idx    = 0;  /* SLOAD bail has no snap */
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;
    /* Restore raw bits into rax (we clobbered it with AND), then store. */
    static const u8 mov_rax_rdx[] = { 0x48, 0x89, 0xD0 };  /* mov rax, rdx */
    cg_emit_bytes(cg, mov_rax_rdx, 3);
    emit_mov_vals_rax(cg, i);
}

/* IR_SSTORE slot, op2: vals[op2] -> frame_slots[slot]. */
static void emit_sstore(CG *cg, u32 slot, u32 op2) {
    emit_mov_rax_vals(cg, op2);
    emit_mov_slot_rax(cg, slot);
}

/* IR_KNUM constpool_ref: load f64 const into vals[i]. */
static void emit_knum(CG *cg, const CandoTraceIR *ir, IRRef k_ref, u32 i) {
    if (!IRREF_IS_K(k_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, k_ref);
    if (!cando_is_number(cv)) { cg->failed = true; return; }
    emit_movabs_rax(cg, cv.u);
    emit_mov_vals_rax(cg, i);
}

/* IR_KBOOL imm: imm in {0,1} -> vals[i] as 0.0 / 1.0. */
static void emit_kbool(CG *cg, u32 imm, u32 i) {
    /* 1.0 in IEEE 754 = 0x3FF0000000000000.  0.0 = 0. */
    u64 bits = (imm != 0) ? 0x3FF0000000000000ULL : 0ULL;
    emit_movabs_rax(cg, bits);
    emit_mov_vals_rax(cg, i);
}

/* Two-operand SSE2 arithmetic: vals[a] OP vals[b] -> vals[i].  IR_DIV
 * is recorded with a NEZ guard immediately preceding it (see OP_DIV
 * in cando_recorder_observe), so the divisor is provably nonzero by
 * the time control reaches divsd here.  No runtime check needed. */
static void emit_arith(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    emit_movsd_xmm_vals(cg, 0, a);
    emit_movsd_xmm_vals(cg, 1, b);
    switch (op) {
    case IR_ADD: emit_addsd(cg); break;
    case IR_SUB: emit_subsd(cg); break;
    case IR_MUL: emit_mulsd(cg); break;
    case IR_DIV: emit_divsd(cg); break;
    default:     cg->failed = true; return;
    }
    emit_movsd_vals_xmm(cg, i, 0);
}

/* IR_NEG: vals[i] = -vals[op1].  Computed as 0 - vals[op1] using the
 * existing subsd-with-zero idiom -- avoids encoding a 64-bit
 * sign-bit-mask constant. */
static void emit_neg(CG *cg, u32 op1, u32 i) {
    emit_xorpd_xmm1_xmm1(cg);                /* xmm1 = 0.0           */
    emit_movsd_xmm_vals(cg, 0, op1);         /* xmm0 = vals[op1]     */
    emit_subsd_xmm1_xmm0(cg);                /* xmm1 = xmm1 - xmm0   */
    emit_movsd_vals_xmm(cg, i, 1);
}

/* Emit the per-guard JNE that targets a placeholder side-exit
 * stub.  Caller has already emitted the comparison whose ZF the
 * branch depends on.  Returns by appending a (je_disp_off,
 * snap_idx) entry to cg->guards so the post-body patch pass can
 * fill in the stub address. */
static void emit_jne_to_stub(CG *cg, u16 snap_idx) {
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x85);    /* JNE rel32 */
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    cg->guards[cg->guard_count].je_disp_off = disp_off;
    cg->guards[cg->guard_count].snap_idx    = snap_idx;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;
}

/* IR_GLOAD: vals[i] = global_by_name (numeric).  Calls the JIT
 * helper which returns 0 on success (writes f64 to *out) or 1 on
 * missing/non-numeric.  On non-zero return we side-exit to roll
 * back any in-flight stores via cur_snap (matches the IR-interp).
 *
 * `name_str` is the trace's interned name -- the codegen looks it
 * up in t->ir.constants[] at emit time and embeds the pointer as
 * an immediate.  Argument layout (SysV):
 *   rdi = vm    rsi = name    rdx = &vals[i]   rax = helper       */
extern int cando_jit_gload_for_mcode(struct CandoVM *vm,
                                     struct CandoString *name,
                                     double *out);
extern int cando_jit_gstore_for_mcode(struct CandoVM *vm,
                                      struct CandoString *name,
                                      double value);

static void emit_gload(CG *cg, const CandoTraceIR *ir, IRRef name_ref, u32 i) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_movabs_rsi(cg, (u64)(uintptr_t)name);    /* arg2: name      */
    emit_lea_rdx_vals(cg, i);                     /* arg3: &vals[i]  */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gload_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
}

static void emit_gstore(CG *cg, const CandoTraceIR *ir, IRRef name_ref,
                        u32 op2) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_movabs_rsi(cg, (u64)(uintptr_t)name);    /* arg2: name      */
    emit_movsd_xmm_vals(cg, 0, op2);              /* arg3 (f64 in xmm0) */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gstore_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
}

/* IR_CALL_F1: vals[i] = fast_native(vals[op2]).  op1 is the index
 * into vm->fast_natives_f1[].  We resolve the function pointer at
 * codegen time and embed it as an immediate -- registrations are
 * write-once at startup so the pointer is stable for the trace's
 * lifetime.  SysV ABI: f64 arg in XMM0, result in XMM0.  Stack is
 * 16-aligned at this point (5 callee-saved pushes from the prologue
 * land RSP at aligned), so `call rax` to the helper is well-formed. */
static void emit_call_f1(CG *cg, u32 native_idx, u32 op2, u32 i) {
    if (!cg->vm || native_idx >= cg->vm->fast_natives_f1_cap ||
        cg->vm->fast_natives_f1[native_idx] == NULL) {
        cg->failed = true;
        return;
    }
    CandoFastFn1 fn = cg->vm->fast_natives_f1[native_idx];
    emit_movsd_xmm_vals(cg, 0, op2);         /* xmm0 = vals[op2]     */
    emit_movabs_rax(cg, (u64)(uintptr_t)fn);
    emit_call_rax(cg);
    emit_movsd_vals_xmm(cg, i, 0);           /* vals[i] = xmm0       */
}

/* Comparisons: vals[a] CMP vals[b] -> vals[i] as 1.0 or 0.0.
 * Mirrors the IR-interp's "(a CMP b) ? 1.0 : 0.0" semantics.  We
 * trust no NaN inputs in v1 (numeric IR by construction). */
static void emit_compare(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    emit_movsd_xmm_vals(cg, 0, a);
    emit_movsd_xmm_vals(cg, 1, b);
    emit_ucomisd_xmm0_xmm1(cg);
    switch (op) {
    case IR_LT:  emit_setb_al(cg);  break;
    case IR_LE:  emit_setbe_al(cg); break;
    case IR_GT:  emit_seta_al(cg);  break;
    case IR_GE:  emit_setae_al(cg); break;
    case IR_EQ:  emit_sete_al(cg);  break;
    case IR_NEQ: emit_setne_al(cg); break;
    default:     cg->failed = true; return;
    }
    emit_movzx_eax_al(cg);
    emit_cvtsi2sd_xmm0_eax(cg);
    emit_movsd_vals_xmm(cg, i, 0);
}

/* IR_GUARD_TRUE / IR_GUARD_FALSE: load vals[op1] as f64, compare to
 * 0.0; if mismatch, jump to per-guard stub (which loads snap_idx and
 * jumps to side_exit_common). */
static void emit_guard_bool(CG *cg, IROp op, u32 op1, u16 snap_idx) {
    emit_movsd_xmm_vals(cg, 0, op1);
    emit_xorpd_xmm1_xmm1(cg);
    emit_ucomisd_xmm0_xmm1(cg);
    /* ucomisd sets ZF=1 iff equal (or NaN; we trust no NaN).
     *   GUARD_TRUE  fires when vals[op1] == 0.0  -> ZF=1 -> JE
     *   GUARD_FALSE fires when vals[op1] != 0.0  -> ZF=0 -> JNE */
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    if (op == IR_GUARD_TRUE) {
        cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x84);    /* JE rel32 */
    } else if (op == IR_GUARD_FALSE) {
        cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x85);    /* JNE rel32 */
    } else {
        cg->failed = true; return;
    }
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    cg->guards[cg->guard_count].je_disp_off = disp_off;
    cg->guards[cg->guard_count].snap_idx    = snap_idx;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;
}

/* ============================================================ */
/* Main entry point                                              */
/* ============================================================ */

bool cando_jit_codegen_trace(struct CandoVM *vm, CandoTrace *t) {
    if (!t || t->mcode_fn != NULL) return t && t->mcode_fn != NULL;
    if (t->ir.ir_count == 0) return false;

    if (!cando_mcode_alloc(&t->mcode, CG_BUF_SIZE)) return false;

    CG cg = (CG){0};
    cg.base = t->mcode.base;
    cg.cur  = t->mcode.base;
    cg.end  = t->mcode.base + t->mcode.size;
    cg.vm   = vm;

    emit_prologue(&cg);

    /* Per-IR-op emit. */
    for (u32 i = 1; i < t->ir.ir_count && !cg.failed; i++) {
        const IRIns *in = &t->ir.ir[i];
        switch (in->op) {
        case IR_NOP:
            /* Skip silently; DSE/DCE may have left these. */
            break;
        case IR_KNUM:
            emit_knum(&cg, &t->ir, in->op1, i);
            break;
        case IR_KBOOL:
            emit_kbool(&cg, in->op1, i);
            break;
        case IR_SLOAD:
            emit_sload(&cg, in->op1, i);
            break;
        case IR_SSTORE:
            emit_sstore(&cg, in->op1, in->op2);
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            emit_arith(&cg, (IROp)in->op, in->op1, in->op2, i);
            break;
        case IR_NEG:
            emit_neg(&cg, in->op1, i);
            break;
        case IR_CALL_F1:
            emit_call_f1(&cg, in->op1, in->op2, i);
            break;
        case IR_EQ: case IR_NEQ: case IR_LT: case IR_LE:
        case IR_GT: case IR_GE:
            emit_compare(&cg, (IROp)in->op, in->op1, in->op2, i);
            break;
        case IR_GUARD_NUM:
            /* No-op at runtime: the SLOAD that produced in->op1 has
             * already type-checked the slot.  Pure-IR ops don't
             * change the type tag, so by the time control reaches a
             * GUARD_NUM the value really is numeric. */
            break;
        case IR_GUARD_TRUE: case IR_GUARD_FALSE:
            emit_guard_bool(&cg, (IROp)in->op, in->op1, (u16)in->op2);
            cg.cur_snap = (u16)in->op2;
            break;
        case IR_GLOAD:
            emit_gload(&cg, &t->ir, in->op1, i);
            break;
        case IR_GSTORE:
            emit_gstore(&cg, &t->ir, in->op1, in->op2);
            break;
        case IR_LOOP:
            /* Trace-close marker; we'll emit the LOOP_DONE epilogue
             * right after the body loop exits. */
            break;
        default:
            /* Op not yet handled by codegen v1.  Bail. */
            cg.failed = true;
            break;
        }
    }

    if (cg.failed) {
        cando_mcode_free(&t->mcode);
        return false;
    }

    /* LOOP_DONE epilogue: returns 0 (TRACE_LOOP_DONE). */
    emit_xor_eax_eax(&cg);
    emit_epilogue(&cg);

    /* Per-guard stubs: each loads its snap_idx into r9d then jumps
     * to side_exit_common.  Records the stub's offset so we can
     * patch the corresponding guard's je/jne afterwards. */
    u32 stub_jmp_offs[CG_MAX_GUARDS] = {0};
    for (u32 g = 0; g < cg.guard_count && !cg.failed; g++) {
        cg.guards[g].stub_off = cg_off(&cg);
        emit_mov_r9d_imm(&cg, cg.guards[g].snap_idx);
        stub_jmp_offs[g] = emit_jmp_rel32_placeholder(&cg);
    }

    /* side_exit_common:
     *   mov rdi, rbx          -- vm
     *   mov rsi, r12          -- t
     *   mov rdx, r14          -- vals
     *   mov rcx, r15          -- frame_slots
     *   mov r8,  r9           -- snap_idx
     *   movabs rax, helper
     *   call rax
     *   mov eax, 1            -- TRACE_GUARD_FAILED
     *   epilogue
     */
    u32 common_off = cg_off(&cg);
    emit_mov_rdi_rbx(&cg);
    emit_mov_rsi_r12(&cg);
    emit_mov_rdx_r14(&cg);
    emit_mov_rcx_r15(&cg);
    emit_mov_r8_r9(&cg);
    emit_movabs_rax(&cg, (u64)(uintptr_t)&cando_jit_replay_snapshot_for_mcode);
    emit_call_rax(&cg);
    emit_mov_eax_imm(&cg, 1);   /* TRACE_GUARD_FAILED */
    emit_epilogue(&cg);

    if (cg.failed) {
        cando_mcode_free(&t->mcode);
        return false;
    }

    /* Patch each guard's je/jne -> stub, and each stub's jmp -> common. */
    for (u32 g = 0; g < cg.guard_count; g++) {
        cg_patch_rel32(&cg, cg.guards[g].je_disp_off, cg.guards[g].stub_off);
        cg_patch_rel32(&cg, stub_jmp_offs[g], common_off);
    }

    t->mcode.written = (u32)(cg.cur - cg.base);
    if (!cando_mcode_finalize(&t->mcode)) {
        cando_mcode_free(&t->mcode);
        return false;
    }
    t->mcode_fn = (CandoTraceStatus (*)(struct CandoVM *, CandoTrace *,
                                        bool, CandoValue *, TraceVal *))
                  (uintptr_t)cg.base;
    return true;
}
