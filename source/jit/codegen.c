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

#define CG_MAX_GUARDS     128
#define CG_MAX_SUNK       16
#define CG_MAX_OBJ_FIELDS 8       /* per-sunk-object field cap */
#define CG_BUF_SIZE       4096u   /* one page; bail if a trace needs more */

/* Phase 4.4k / v1: sunk-allocation tracking.  Either:
 *   IS_ARRAY=true  -- backing buffer indexed 0..capacity-1; APPEND
 *                     uses cursor++; INDEX_GET/SET uses constant idx.
 *   IS_ARRAY=false -- backing buffer indexed by field-name slot
 *                     assigned in field_kref[] order; FIELD_SET
 *                     registers a new slot if the name is unseen,
 *                     FIELD_GET requires a previously-set name.
 * Both share stack_off = base offset of slot 0 from rbp. */
typedef struct {
    u32 ref;            /* IRRef of the IR_NEW_ARRAY/IR_NEW_OBJECT     */
    u32 capacity;       /* slots reserved (array: op1; object: max #f) */
    u32 cursor;         /* array: append index.  object: # discovered  */
    i32 stack_off;      /* negative offset from rbp to slot 0          */
    bool is_array;      /* false for sunk IR_NEW_OBJECT                */
    /* Object only: KIDX of each known field name in slot order.       */
    u32 field_kref[CG_MAX_OBJ_FIELDS];
} CGSunk;

typedef struct {
    u8  *base;          /* start of mcode buffer */
    u8  *cur;           /* current write pointer */
    u8  *end;           /* one past the last writable byte */
    bool failed;        /* set on overflow or unsupported op */
    struct CandoVM *vm; /* threaded through for fast-native lookup */
    u16 cur_snap;       /* most-recent guard's snapshot index, used by
                           IR_GLOAD/IR_GSTORE bad-type side-exits to
                           match the IR-interpreter's behaviour */
    /* Phase 4.4k: sunk allocations get a stack-local buffer instead
     * of a heap call.  Filled by a pre-pass before the body emits;
     * indexed by IRRef during emit. */
    CGSunk sunk[CG_MAX_SUNK];
    u32    sunk_count;
    u32    sunk_total_bytes;   /* total stack to reserve, 16-aligned   */

    u32 xmm0_holds;     /* Phase 6.8: IRRef whose vals[] value is
                           currently in xmm0 (0 = unknown).  Lets
                           consecutive non-invariant ops skip a
                           redundant `movsd xmm0, [vals+...]` load
                           when the next op needs the same IRRef.
                           Invalidated on C calls, on invariant-op
                           emit (the LICM skip-prefix may bypass
                           the producer at runtime), and at side-
                           exit boundaries. */

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

/* Phase 6.8: load vals[idx] into xmm0 unless it's already there.
 * Caller is responsible for setting cg->xmm0_holds = idx after a
 * successful op-result store (so the next op can hit the cache). */
static void emit_load_xmm0(CG *cg, u32 idx);

/* Invalidate the xmm0 cache.  Called after any op that clobbers
 * xmm0 unpredictably (C calls, all four GLOAD/GSTORE/HLOAD_SLOT/
 * AREF helpers).  Also called before/after invariant ops because
 * the LICM skip-prefix may bypass the producer at runtime. */
static void cg_invalidate_xmm0(CG *cg) { cg->xmm0_holds = 0; }

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
static void emit_load_xmm0(CG *cg, u32 idx) {
    if (cg->xmm0_holds == idx && idx != 0) return;
    emit_movsd_xmm_vals(cg, 0, idx);
    cg->xmm0_holds = idx;
}

static void emit_movsd_vals_xmm(CG *cg, u32 idx, u8 xmm) {
    cg_emit_u8(cg, 0xF2);
    cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x11);
    cg_emit_u8(cg, (u8)(0x86 | (xmm << 3)));
    cg_emit_u32(cg, idx * 8);
}

/* movsd xmm0, [rbp + disp32]  -- F2 0F 10 85 disp32.  Phase 4.4k
 * sunk-buffer reads.  disp is signed; negative = below rbp. */
static void emit_movsd_xmm0_rbp(CG *cg, i32 disp) {
    static const u8 prefix[] = { 0xF2, 0x0F, 0x10, 0x85 };
    cg_emit_bytes(cg, prefix, 4);
    cg_emit_u32(cg, (u32)disp);
}
/* movsd [rbp + disp32], xmm0  -- F2 0F 11 85 disp32.  Sunk-buffer
 * writes. */
static void emit_movsd_rbp_xmm0(CG *cg, i32 disp) {
    static const u8 prefix[] = { 0xF2, 0x0F, 0x11, 0x85 };
    cg_emit_bytes(cg, prefix, 4);
    cg_emit_u32(cg, (u32)disp);
}

/* Lookup a sunk allocation by its IRRef.  Returns NULL if not sunk. */
static CGSunk *cg_find_sunk(CG *cg, u32 ref) {
    for (u32 k = 0; k < cg->sunk_count; k++)
        if (cg->sunk[k].ref == ref) return &cg->sunk[k];
    return NULL;
}

/* Pre-pass: scan IR for IRF_SUNK IR_NEW_ARRAY / IR_NEW_OBJECT ops
 * and assign each a stack offset + capacity.
 *
 * For arrays the capacity is the IR's op1 (the OP_NEW_ARRAY count
 * argument) -- known at recording time.
 *
 * For objects we scan forward and count UNIQUE field-name KIDX
 * values across all IR_FIELD_SET ops referencing this alloc.  That
 * gives the object's slot count.  Bails (drops SUNK) if more than
 * CG_MAX_OBJ_FIELDS unique fields. */
static void cg_assign_sunk_offsets(CG *cg, const CandoTraceIR *ir) {
    u32 running = 0;
    for (u32 i = 1; i < ir->ir_count; i++) {
        const IRIns *in = &ir->ir[i];
        bool is_array  = (in->op == IR_NEW_ARRAY);
        bool is_object = (in->op == IR_NEW_OBJECT);
        if ((!is_array && !is_object) || !(in->flags & IRF_SUNK)) continue;
        if (cg->sunk_count >= CG_MAX_SUNK) {
            ((IRIns *)in)->flags &= (u8)~IRF_SUNK;
            continue;
        }

        u32 capacity;
        u32 field_count = 0;
        u32 fields[CG_MAX_OBJ_FIELDS];

        if (is_array) {
            capacity = in->op1;
        } else {
            /* Object: scan forward for FIELD_SET ops on this alloc. */
            bool overflowed = false;
            for (u32 j = i + 1; j < ir->ir_count && !overflowed; j++) {
                const IRIns *u = &ir->ir[j];
                if (u->op != IR_FIELD_SET || u->op1 != i) continue;
                /* Is u->op2 already in fields[]? */
                bool seen = false;
                for (u32 f = 0; f < field_count; f++)
                    if (fields[f] == u->op2) { seen = true; break; }
                if (seen) continue;
                if (field_count >= CG_MAX_OBJ_FIELDS) { overflowed = true; break; }
                fields[field_count++] = u->op2;
            }
            if (overflowed || field_count == 0) {
                ((IRIns *)in)->flags &= (u8)~IRF_SUNK;
                continue;
            }
            capacity = field_count;
        }

        CGSunk *s = &cg->sunk[cg->sunk_count++];
        s->ref       = i;
        s->capacity  = capacity;
        s->cursor    = 0;
        s->is_array  = is_array;
        s->stack_off = -(i32)(56 + running);
        for (u32 f = 0; f < field_count; f++) s->field_kref[f] = fields[f];
        running += capacity * 8;
    }
    cg->sunk_total_bytes = running;
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
/* movabs rdx, imm64          -- 48 BA imm64. */
static void emit_movabs_rdx(CG *cg, u64 imm) {
    cg_emit_u8(cg, 0x48);
    cg_emit_u8(cg, 0xBA);
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
/* test rax, rax              -- 48 85 C0.  Sets ZF=1 iff rax==0. */
static void emit_test_rax_rax(CG *cg) {
    static const u8 b[] = { 0x48, 0x85, 0xC0 };
    cg_emit_bytes(cg, b, 3);
}
/* mov rsi, r15               -- 4C 89 FE (src=r15, dst=rsi). */
static void emit_mov_rsi_r15(CG *cg) {
    static const u8 b[] = { 0x4C, 0x89, 0xFE };
    cg_emit_bytes(cg, b, 3);
}
/* mov edx, imm32             -- BA imm32. */
static void emit_mov_edx_imm(CG *cg, u32 imm) {
    cg_emit_u8(cg, 0xBA);
    cg_emit_u32(cg, imm);
}
/* mov rdi, [r14 + 8*idx]     -- read u64 from vals[idx] into rdi.
 * REX = 0x49 (W=1 B=1 for r14 base).  Opcode 8B /r.  ModR/M:
 *   mod=10 (disp32), reg=rdi=111, r/m=110 (r14&7) -> 10 111 110 = 0xBE */
static void emit_mov_rdi_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x8B, 0xBE };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* cvttsd2si esi, xmm0        -- F2 0F 2C F0.  Truncate-toward-zero
 * f64 in xmm0 to i32 in esi (which we treat as u32 idx for AREF). */
static void emit_cvttsd2si_esi_xmm0(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x2C, 0xF0 };
    cg_emit_bytes(cg, b, 4);
}
/* cvttsd2si edx, xmm0        -- F2 0F 2C D0. */
static void emit_cvttsd2si_edx_xmm0(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x2C, 0xD0 };
    cg_emit_bytes(cg, b, 4);
}
/* cvttsd2si edx, xmm1        -- F2 0F 2C D1.  Used by IR_INDEX_SET
 * codegen which loads idx into xmm1 (xmm0 is reserved for the
 * value f64 arg). */
static void emit_cvttsd2si_edx_xmm1(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x2C, 0xD1 };
    cg_emit_bytes(cg, b, 4);
}
/* movsd xmm1, [r14 + 8*idx]  -- 4.4g IR_INDEX_SET needs idx in xmm1. */
static void emit_movsd_xmm1_vals(CG *cg, u32 idx) {
    cg_emit_u8(cg, 0xF2); cg_emit_u8(cg, 0x41); cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x10); cg_emit_u8(cg, 0x8E);
    cg_emit_u32(cg, idx * 8);
}
/* mov rsi, [r14 + 8*idx]     -- load array u64 handle into rsi. */
static void emit_mov_rsi_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x8B, 0xB6 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* lea rcx, [r14 + 8*idx]     -- compute &vals[idx] into rcx. */
static void emit_lea_rcx_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x8D, 0x8E };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* je rel32 to a per-guard stub (mirror of emit_jne_to_stub). */
static void emit_je_to_stub(CG *cg, u16 snap_idx);

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
/* Self-op variants of arithmetic (xmm0 OP= xmm0).  Used by the
 * Phase 6.7 peephole when both operands of a binary op are the
 * same IRRef -- one load + one self-op instead of two loads + a
 * cross-register op. */
static void emit_addsd_self(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x58, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}
static void emit_subsd_self(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x5C, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}
static void emit_mulsd_self(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x59, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}
static void emit_divsd_self(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x5E, 0xC0 };
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
 *   push rbp / mov rbp, rsp / push rbx/r12/r13/r14/r15
 *   sub rsp, 8 + sunk_total       (16-aligned)
 *   mov rbx,rdi / mov r12,rsi / mov r13b,dl / mov r15,rcx / mov r14,r8
 *
 * Stack alignment: caller is 16-aligned before `call mcode_fn`;
 * the call+six pushes leave RSP at aligned-56 (8-misaligned), so
 * we subtract enough to re-align to 16.  Total reservation is
 * 8 + sunk_total_bytes where sunk_total is rounded up to a multiple
 * of 8 to keep slot accesses 8-byte aligned and the +8 brings the
 * grand total (56 + 8 + sunk_total) to a multiple of 16.
 *
 * Phase 6.6: r13b mirrors the skip_invariant arg byte for the
 * LICM-skip prefix (body emits clobber RDX so we cache).
 *
 * Phase 4.4k: sunk_total_bytes lives below the 8-byte alignment
 * pad, accessed via [rbp - 56 - 8*N] for the Nth element of the
 * Nth sunk allocation (per-alloc base offsets stored in cg->sunk[]). */
static void emit_prologue(CG *cg) {
    static const u8 fixed1[] = {
        0x55,
        0x48, 0x89, 0xE5,
        0x53,
        0x41, 0x54,
        0x41, 0x55,
        0x41, 0x56,
        0x41, 0x57,
    };
    cg_emit_bytes(cg, fixed1, sizeof(fixed1));

    /* sub rsp, 8 + sunk_total.  Round (8 + sunk_total) up to a
     * multiple of 16 so RSP stays aligned. */
    u32 sub_amt = 8 + cg->sunk_total_bytes;
    if (sub_amt % 16 != 0) sub_amt = ((sub_amt + 15) / 16) * 16;
    if (sub_amt < 0x80) {
        /* sub rsp, imm8: 48 83 EC ib */
        cg_emit_u8(cg, 0x48); cg_emit_u8(cg, 0x83);
        cg_emit_u8(cg, 0xEC); cg_emit_u8(cg, (u8)sub_amt);
    } else {
        /* sub rsp, imm32: 48 81 EC id */
        cg_emit_u8(cg, 0x48); cg_emit_u8(cg, 0x81);
        cg_emit_u8(cg, 0xEC); cg_emit_u32(cg, sub_amt);
    }

    static const u8 fixed2[] = {
        0x48, 0x89, 0xFB,
        0x49, 0x89, 0xF4,
        0x41, 0x88, 0xD5,
        0x49, 0x89, 0xCF,
        0x4D, 0x89, 0xC6,
    };
    cg_emit_bytes(cg, fixed2, sizeof(fixed2));
}

/* Epilogue: undo the prologue's sub rsp + pops + ret. */
static void emit_epilogue(CG *cg) {
    u32 sub_amt = 8 + cg->sunk_total_bytes;
    if (sub_amt % 16 != 0) sub_amt = ((sub_amt + 15) / 16) * 16;
    if (sub_amt < 0x80) {
        cg_emit_u8(cg, 0x48); cg_emit_u8(cg, 0x83);
        cg_emit_u8(cg, 0xC4); cg_emit_u8(cg, (u8)sub_amt);
    } else {
        cg_emit_u8(cg, 0x48); cg_emit_u8(cg, 0x81);
        cg_emit_u8(cg, 0xC4); cg_emit_u32(cg, sub_amt);
    }
    static const u8 b[] = {
        0x41, 0x5F,                /* pop r15        */
        0x41, 0x5E,                /* pop r14        */
        0x41, 0x5D,                /* pop r13        */
        0x41, 0x5C,                /* pop r12        */
        0x5B,                      /* pop rbx        */
        0x5D,                      /* pop rbp        */
        0xC3,                      /* ret            */
    };
    cg_emit_bytes(cg, b, sizeof(b));
}

/* IR_SLOAD slot: load frame_slots[slot] into rax, type-check, store
 * to vals[i].  IRT_NUM checks (rax & NB_MASK) != NB_MASK; IRT_OBJ
 * checks (rax & NB_TAG_BITS_MASK) == NB_TAG_OBJECT.  Side-exits
 * (snap=0) on type mismatch. */
static void emit_sload(CG *cg, u32 slot, u32 i, IRType slot_type) {
    emit_mov_rax_slot(cg, slot);
    if (slot_type == IRT_OBJ) {
        emit_movabs_rcx(cg, 0xFFFF000000000000ULL);   /* NB_TAG_BITS_MASK */
        emit_mov_rdx_rax(cg);
        emit_and_rax_rcx(cg);
        emit_movabs_rcx(cg, 0xFFFC000000000000ULL);   /* NB_TAG_OBJECT   */
        emit_cmp_rax_rcx(cg);
        /* JNE side_exit on (high16 & MASK) != OBJECT_TAG */
        if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
        cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x85);   /* JNE rel32 */
        u32 disp_off = cg_off(cg);
        cg_emit_u32(cg, 0);
        cg->guards[cg->guard_count].je_disp_off = disp_off;
        cg->guards[cg->guard_count].snap_idx    = 0;
        cg->guards[cg->guard_count].stub_off    = 0;
        cg->guard_count++;
    } else {
        emit_movabs_rcx(cg, 0xFFF8000000000000ULL);   /* NB_MASK */
        emit_mov_rdx_rax(cg);
        emit_and_rax_rcx(cg);                         /* rax = v.u & MASK */
        emit_cmp_rax_rcx(cg);
        /* je side_exit (boxed value) */
        if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
        cg->guards[cg->guard_count].je_disp_off = emit_je_rel32_placeholder(cg);
        cg->guards[cg->guard_count].snap_idx    = 0;
        cg->guards[cg->guard_count].stub_off    = 0;
        cg->guard_count++;
    }
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
 * the time control reaches divsd here.  No runtime check needed.
 *
 * Phase 6.7 peephole: when a == b (e.g. squaring `x * x`), load
 * once into xmm0 and use the self-op variant -- saves one 9-byte
 * movsd load.  Mandelbrot's hot loop has two such MULs (zr*zr,
 * zi*zi) per iteration. */
static void emit_arith(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    emit_load_xmm0(cg, a);
    if (a == b) {
        switch (op) {
        case IR_ADD: emit_addsd_self(cg); break;
        case IR_SUB: emit_subsd_self(cg); break;
        case IR_MUL: emit_mulsd_self(cg); break;
        case IR_DIV: emit_divsd_self(cg); break;
        default:     cg->failed = true; return;
        }
    } else {
        emit_movsd_xmm_vals(cg, 1, b);
        switch (op) {
        case IR_ADD: emit_addsd(cg); break;
        case IR_SUB: emit_subsd(cg); break;
        case IR_MUL: emit_mulsd(cg); break;
        case IR_DIV: emit_divsd(cg); break;
        default:     cg->failed = true; return;
        }
    }
    emit_movsd_vals_xmm(cg, i, 0);
    cg->xmm0_holds = i;
}

/* ============================================================ */
/* Phase 4.4g: array allocation + index access codegen           */
/* ============================================================ */

static void emit_jne_to_stub(CG *cg, u16 snap_idx);   /* fwd decl */

extern u64 cando_jit_new_array_for_mcode(struct CandoVM *vm);
extern int cando_jit_array_append_for_mcode(struct CandoVM *vm, u64 arr_u,
                                             double val);
extern int cando_jit_index_get_for_mcode(struct CandoVM *vm, u64 arr_u, u32 idx,
                                          double *out);
extern int cando_jit_index_set_for_mcode(struct CandoVM *vm, u64 arr_u, u32 idx,
                                          double val);

/* IR_NEW_ARRAY: vals[i].u = cando_jit_new_array(vm).  Single-arg
 * (vm in rdi); no failure path -- the helper always returns a
 * fresh handle. */
static void emit_new_array(CG *cg, u32 i) {
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_new_array_for_mcode);
    emit_call_rax(cg);
    emit_mov_vals_rax(cg, i);                     /* vals[i].u = rax */
    cg_invalidate_xmm0(cg);
}

/* IR_ARRAY_APPEND: cando_jit_array_append(vm, vals[op1].u, vals[op2].d).
 * SysV: vm in rdi, arr in rsi, val in xmm0.  Returns int 0/1. */
static void emit_array_append(CG *cg, u32 op1, u32 op2) {
    emit_mov_rdi_rbx(cg);
    emit_mov_rsi_vals(cg, op1);                   /* arg2: arr u64   */
    emit_load_xmm0(cg, op2);                      /* arg3: val f64   */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_array_append_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);
}

/* IR_INDEX_GET: cando_jit_index_get(vm, vals[op1].u, (u32)vals[op2].d, &vals[i]).
 * Returns 0/1; on 1 we side-exit. */
static void emit_index_get(CG *cg, u32 op1, u32 op2, u32 i) {
    emit_mov_rdi_rbx(cg);
    emit_mov_rsi_vals(cg, op1);                   /* arg2: arr u64   */
    emit_load_xmm0(cg, op2);                      /* xmm0 = idx f64  */
    emit_cvttsd2si_edx_xmm0(cg);                  /* arg3: u32 idx   */
    emit_lea_rcx_vals(cg, i);                     /* arg4: &vals[i]  */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_index_get_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);
}

/* IR_INDEX_SET: cando_jit_index_set(vm, vals[op1].u, idx, val).  The
 * value comes from the preceding IR_INDEX_SET_VAL (i-1 convention).
 * Args: vm/rdi, arr/rsi, idx/edx, val/xmm0. */
static void emit_index_set(CG *cg, u32 op1, u32 op2, u32 val_ref) {
    emit_mov_rdi_rbx(cg);
    emit_mov_rsi_vals(cg, op1);
    emit_movsd_xmm1_vals(cg, op2);                /* xmm1 = idx f64  */
    emit_cvttsd2si_edx_xmm1(cg);                  /* arg3: u32 idx   */
    /* Load val LAST so xmm0 holds it at call time. */
    cg_invalidate_xmm0(cg);                       /* xmm1-load + cvt may have shifted state */
    emit_load_xmm0(cg, val_ref);
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_index_set_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);
}

/* ============================================================ */
/* Phase 4.4h: object allocation + field access codegen          */
/* ============================================================ */

extern u64 cando_jit_new_object_for_mcode(struct CandoVM *vm);
extern int cando_jit_field_set_for_mcode(struct CandoVM *vm, u64 obj_u,
                                          struct CandoString *name, double val);
extern int cando_jit_field_get_for_mcode(struct CandoVM *vm, u64 obj_u,
                                          struct CandoString *name, double *out);

/* IR_NEW_OBJECT: vals[i].u = cando_jit_new_object(vm). */
static void emit_new_object(CG *cg, u32 i) {
    emit_mov_rdi_rbx(cg);
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_new_object_for_mcode);
    emit_call_rax(cg);
    emit_mov_vals_rax(cg, i);
    cg_invalidate_xmm0(cg);
}

/* IR_FIELD_SET: cando_jit_field_set(vm, vals[op1].u, name_str, val).
 * Args: vm/rdi, obj/rsi, name/rdx, val/xmm0.  val_ref from i-1
 * IR_FIELD_SET_VAL. */
static void emit_field_set(CG *cg, u32 op1, IRRef name_kref, u32 val_ref,
                           const CandoTraceIR *ir) {
    CandoValue cv = cando_ir_get_const(ir, name_kref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);
    emit_mov_rsi_vals(cg, op1);
    emit_movabs_rdx(cg, (u64)(uintptr_t)name);    /* arg3: name      */
    emit_load_xmm0(cg, val_ref);                  /* arg4: val (xmm0) */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_field_set_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);
}

/* IR_FIELD_GET: cando_jit_field_get(vm, vals[op1].u, name, &vals[i]). */
static void emit_field_get(CG *cg, u32 op1, IRRef name_kref, u32 i,
                           const CandoTraceIR *ir) {
    CandoValue cv = cando_ir_get_const(ir, name_kref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);
    emit_mov_rsi_vals(cg, op1);
    emit_movabs_rdx(cg, (u64)(uintptr_t)name);
    emit_lea_rcx_vals(cg, i);                     /* arg4: &vals[i]  */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_field_get_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);
}

/* ============================================================ */
/* Phase 4.4i: range allocation codegen                          */
/* ============================================================ */

extern u64 cando_jit_range_asc_for_mcode(struct CandoVM *vm,
                                          double from, double to);
extern u64 cando_jit_range_desc_for_mcode(struct CandoVM *vm,
                                           double from, double to);

/* movsd xmm1, xmm0 -- F2 0F 10 C8.  Used when both args need the
 * same xmm-cached value moved to a different register. */
static void emit_movsd_xmm1_xmm0(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x10, 0xC8 };
    cg_emit_bytes(cg, b, 4);
}

/* IR_RANGE_ASC / IR_RANGE_DESC: vals[i].u = helper(vm, from, to).
 * SysV: vm/rdi, from/xmm0, to/xmm1.  Always succeeds. */
static void emit_range(CG *cg, IROp op, u32 from_ref, u32 to_ref, u32 i) {
    emit_mov_rdi_rbx(cg);
    emit_load_xmm0(cg, from_ref);
    /* Load `to` into xmm1.  If from == to we'd need same value in
     * both -- emit_movsd_xmm1_xmm0 handles that without re-reading
     * from memory. */
    if (from_ref == to_ref) {
        emit_movsd_xmm1_xmm0(cg);
    } else {
        emit_movsd_xmm1_vals(cg, to_ref);
    }
    void *fn = (op == IR_RANGE_ASC)
                ? (void *)&cando_jit_range_asc_for_mcode
                : (void *)&cando_jit_range_desc_for_mcode;
    emit_movabs_rax(cg, (u64)(uintptr_t)fn);
    emit_call_rax(cg);
    emit_mov_vals_rax(cg, i);
    cg_invalidate_xmm0(cg);
}

/* IR_NEG: vals[i] = -vals[op1].  Computed as 0 - vals[op1] using the
 * existing subsd-with-zero idiom -- avoids encoding a 64-bit
 * sign-bit-mask constant. */
static void emit_neg(CG *cg, u32 op1, u32 i) {
    emit_xorpd_xmm1_xmm1(cg);                /* xmm1 = 0.0           */
    emit_load_xmm0(cg, op1);                 /* xmm0 = vals[op1]     */
    emit_subsd_xmm1_xmm0(cg);                /* xmm1 = xmm1 - xmm0   */
    emit_movsd_vals_xmm(cg, i, 1);
    /* xmm0 still holds op1; vals[i] holds -op1 (via xmm1). */
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
/* Mirror for JE (used by IR_HLOAD_SLOT's NULL-pointer side-exit). */
static void emit_je_to_stub(CG *cg, u16 snap_idx) {
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x84);    /* JE rel32 */
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
    cg_invalidate_xmm0(cg);                       /* call clobbered xmm0 */
}

/* IR_HLOAD_SLOT: vals[i].p = cando_jit_hload_slot(vm, frame_slots, slot).
 * The helper resolves the OBJECT handle at frame_slots[slot] to an
 * OBJ_ARRAY CdoObject* (or NULL on bad type).  We side-exit on NULL
 * via cur_snap. */
extern void *cando_jit_hload_slot_for_mcode(struct CandoVM *vm,
                                             CandoValue *frame_slots,
                                             u32 slot);
extern int   cando_jit_aref_for_mcode(void *arr, u32 idx, double *out);

static void emit_hload_slot(CG *cg, u32 slot, u32 i) {
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_mov_rsi_r15(cg);                         /* arg2: frame_slots */
    emit_mov_edx_imm(cg, slot);                   /* arg3: slot      */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_hload_slot_for_mcode);
    emit_call_rax(cg);
    emit_test_rax_rax(cg);
    emit_je_to_stub(cg, cg->cur_snap);            /* NULL -> bad type */
    emit_mov_vals_rax(cg, i);                     /* vals[i] = ptr   */
    cg_invalidate_xmm0(cg);                       /* call clobbered xmm0 */
}

/* IR_AREF: vals[i].d = cando_jit_aref(vals[op1].p, (u32)vals[op2].d).
 * Helper returns 0/f64-out on success, 1/bad-type on failure.
 *
 * Argument layout:
 *   rdi = arr (loaded from vals[op1] as raw u64 pointer)
 *   esi = idx (cvttsd2si of vals[op2])
 *   rdx = &vals[i] (out pointer for the f64 result) */
static void emit_aref(CG *cg, u32 op1, u32 op2, u32 i) {
    emit_mov_rdi_vals(cg, op1);                   /* arr ptr         */
    emit_load_xmm0(cg, op2);                      /* xmm0 = idx (f64) */
    emit_cvttsd2si_esi_xmm0(cg);                  /* esi = (i32)xmm0 */
    emit_lea_rdx_vals(cg, i);                     /* &vals[i]        */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_aref_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);                       /* call clobbered xmm0 */
}

static void emit_gstore(CG *cg, const CandoTraceIR *ir, IRRef name_ref,
                        u32 op2) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_movabs_rsi(cg, (u64)(uintptr_t)name);    /* arg2: name      */
    emit_load_xmm0(cg, op2);                      /* arg3 (f64 in xmm0) */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gstore_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);
    cg_invalidate_xmm0(cg);                       /* call clobbered xmm0 */
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
    emit_load_xmm0(cg, op2);                 /* xmm0 = vals[op2]     */
    emit_movabs_rax(cg, (u64)(uintptr_t)fn);
    emit_call_rax(cg);
    emit_movsd_vals_xmm(cg, i, 0);           /* vals[i] = xmm0       */
    cg->xmm0_holds = i;
}

/* Comparisons: vals[a] CMP vals[b] -> vals[i] as 1.0 or 0.0.
 * Mirrors the IR-interp's "(a CMP b) ? 1.0 : 0.0" semantics.  We
 * trust no NaN inputs in v1 (numeric IR by construction). */
static void emit_compare(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    emit_load_xmm0(cg, a);
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
    cg->xmm0_holds = i;
}

/* IR_GUARD_TRUE / IR_GUARD_FALSE: load vals[op1] as f64, compare to
 * 0.0; if mismatch, jump to per-guard stub (which loads snap_idx and
 * jumps to side_exit_common). */
static void emit_guard_bool(CG *cg, IROp op, u32 op1, u16 snap_idx) {
    emit_load_xmm0(cg, op1);
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

    /* Phase 4.4k: pre-pass to assign stack offsets to sunk allocs
     * BEFORE the prologue so its sub rsp can reserve the right
     * amount.  Reads IRF_SUNK set by escape_analysis. */
    cg_assign_sunk_offsets(&cg, &t->ir);

    emit_prologue(&cg);

    /* Per-IR-op emit.  LICM-aware: ops marked IRF_INVARIANT get a
     * `test dl, dl; jnz +op_size` prefix so iter-2+ calls
     * (skip_invariant=true, DL=1) jump over them.  vals[i] is reused
     * from iter 1 in that case -- same contract as the IR-interp.
     * Disp8 caps the per-op size at 127 bytes; if any single op
     * doesn't fit (it shouldn't with the v1 op set), codegen bails. */
    for (u32 i = 1; i < t->ir.ir_count && !cg.failed; i++) {
        const IRIns *in = &t->ir.ir[i];

        bool is_inv = (in->flags & IRF_INVARIANT) &&
                      in->op != IR_NOP && in->op != IR_LOOP;
        u32 skip_disp_off = 0;
        if (is_inv) {
            /* test r13b, r13b -- 45 84 ED.  Tests our cached
             * skip_invariant byte (Phase 6.6); body emits clobber
             * RDX/DL freely, so we can't read DL directly here. */
            static const u8 prefix[] = { 0x45, 0x84, 0xED, 0x75 };
            cg_emit_bytes(&cg, prefix, 4);   /* test r13b,r13b ; jnz disp8 */
            skip_disp_off = cg_off(&cg);
            cg_emit_u8(&cg, 0);
            /* Phase 6.8: the JNZ may bypass the producer at runtime,
             * so the xmm0 cache state across an invariant op is
             * unknown.  Invalidate before AND after. */
            cg_invalidate_xmm0(&cg);
        }
        u32 op_start = cg_off(&cg);

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
            /* Phase 4.4g: handle both IRT_NUM and IRT_OBJ.  Other
             * types (IRT_STR, IRT_PTR resolved object) still bail. */
            if (in->type != IRT_NUM && in->type != IRT_OBJ) {
                cg.failed = true; break;
            }
            emit_sload(&cg, in->op1, i, (IRType)in->type);
            break;
        case IR_SSTORE:
            /* Phase 4.4g: raw u64 copy works for both IRT_NUM and
             * IRT_OBJ source -- emit_sstore is just mov rax mem.
             * NaN canonicalization (cando_number) was a pre-existing
             * gap on the codegen path; not regressed here. */
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
            /* Phase 4.4d: only IRT_NUM globals are codegen'd today;
             * IRT_OBJ globals (script object handles) need a different
             * type-check encoding that lands with Phase 4.4g. */
            if (in->type != IRT_NUM) { cg.failed = true; break; }
            emit_gload(&cg, &t->ir, in->op1, i);
            break;
        case IR_GSTORE:
            emit_gstore(&cg, &t->ir, in->op1, in->op2);
            break;
        case IR_HLOAD_SLOT:
            emit_hload_slot(&cg, in->op1, i);
            break;
        case IR_AREF:
            emit_aref(&cg, in->op1, in->op2, i);
            break;
        case IR_NEW_ARRAY:
            /* Phase 4.4k: sunk allocs emit nothing -- the buffer is
             * already reserved by the prologue's sub rsp. */
            if (in->flags & IRF_SUNK) break;
            emit_new_array(&cg, i);
            break;
        case IR_ARRAY_APPEND: {
            /* Phase 4.4k: if op1 references a sunk alloc, lower to
             * a direct buffer write at slot[cursor++]. */
            CGSunk *s = cg_find_sunk(&cg, in->op1);
            if (s) {
                if (s->cursor >= s->capacity) { cg.failed = true; break; }
                emit_load_xmm0(&cg, in->op2);
                i32 disp = s->stack_off - (i32)(8 * s->cursor);
                emit_movsd_rbp_xmm0(&cg, disp);
                s->cursor++;
                cg_invalidate_xmm0(&cg);
                break;
            }
            emit_array_append(&cg, in->op1, in->op2);
            break;
        }
        case IR_INDEX_GET: {
            /* Phase 4.4k: sunk path requires a constant idx that
             * fits within the buffer.  Variable-idx access falls
             * back to the helper-call path -- but a sunk alloc has
             * no real handle, so falling back is wrong.  Bail in
             * that case so the trace runs via IR-interp. */
            CGSunk *s = cg_find_sunk(&cg, in->op1);
            if (s) {
                const IRIns *idx_in = cando_ir_get_ins(&t->ir, in->op2);
                if (!idx_in || idx_in->op != IR_KNUM) {
                    cg.failed = true; break;
                }
                CandoValue idx_val = cando_ir_get_const(&t->ir, idx_in->op1);
                if (!cando_is_number(idx_val)) { cg.failed = true; break; }
                u32 idx = (u32)cando_as_number(idx_val);
                if (idx >= s->capacity) { cg.failed = true; break; }
                i32 disp = s->stack_off - (i32)(8 * idx);
                emit_movsd_xmm0_rbp(&cg, disp);
                emit_movsd_vals_xmm(&cg, i, 0);
                cg.xmm0_holds = i;
                break;
            }
            emit_index_get(&cg, in->op1, in->op2, i);
            break;
        }
        case IR_INDEX_SET_VAL:
            /* No-op at runtime; the value IRRef is consumed by the
             * IR_INDEX_SET that immediately follows via i-1. */
            break;
        case IR_INDEX_SET: {
            /* Read the value IRRef from the preceding IR_INDEX_SET_VAL. */
            if (i == 0 || t->ir.ir[i - 1].op != IR_INDEX_SET_VAL) {
                cg.failed = true; break;
            }
            u32 val_ref = t->ir.ir[i - 1].op1;
            CGSunk *s = cg_find_sunk(&cg, in->op1);
            if (s) {
                const IRIns *idx_in = cando_ir_get_ins(&t->ir, in->op2);
                if (!idx_in || idx_in->op != IR_KNUM) {
                    cg.failed = true; break;
                }
                CandoValue idx_val = cando_ir_get_const(&t->ir, idx_in->op1);
                if (!cando_is_number(idx_val)) { cg.failed = true; break; }
                u32 idx = (u32)cando_as_number(idx_val);
                if (idx >= s->capacity) { cg.failed = true; break; }
                i32 disp = s->stack_off - (i32)(8 * idx);
                emit_load_xmm0(&cg, val_ref);
                emit_movsd_rbp_xmm0(&cg, disp);
                cg_invalidate_xmm0(&cg);
                break;
            }
            emit_index_set(&cg, in->op1, in->op2, val_ref);
            break;
        }
        case IR_NEW_OBJECT:
            /* Sunk objects skip the helper call -- their backing
             * memory is in the prologue's stack reservation. */
            if (in->flags & IRF_SUNK) break;
            emit_new_object(&cg, i);
            break;
        case IR_FIELD_GET: {
            CGSunk *s = cg_find_sunk(&cg, in->op1);
            if (s && !s->is_array) {
                /* Look up the field name (op2 KREF) in s->field_kref[]. */
                u32 slot = (u32)-1;
                for (u32 f = 0; f < s->capacity; f++)
                    if (s->field_kref[f] == in->op2) { slot = f; break; }
                if (slot == (u32)-1) {
                    /* Field never SET on this sunk obj; bail. */
                    cg.failed = true; break;
                }
                i32 disp = s->stack_off - (i32)(8 * slot);
                emit_movsd_xmm0_rbp(&cg, disp);
                emit_movsd_vals_xmm(&cg, i, 0);
                cg.xmm0_holds = i;
                break;
            }
            emit_field_get(&cg, in->op1, in->op2, i, &t->ir);
            break;
        }
        case IR_FIELD_SET_VAL:
            /* Pair-prefix no-op; FIELD_SET reads from i-1. */
            break;
        case IR_FIELD_SET: {
            if (i == 0 || t->ir.ir[i - 1].op != IR_FIELD_SET_VAL) {
                cg.failed = true; break;
            }
            u32 val_ref = t->ir.ir[i - 1].op1;
            CGSunk *s = cg_find_sunk(&cg, in->op1);
            if (s && !s->is_array) {
                /* Find the slot for this field name (pre-pass already
                 * populated s->field_kref[]). */
                u32 slot = (u32)-1;
                for (u32 f = 0; f < s->capacity; f++)
                    if (s->field_kref[f] == in->op2) { slot = f; break; }
                if (slot == (u32)-1) { cg.failed = true; break; }
                i32 disp = s->stack_off - (i32)(8 * slot);
                emit_load_xmm0(&cg, val_ref);
                emit_movsd_rbp_xmm0(&cg, disp);
                cg_invalidate_xmm0(&cg);
                break;
            }
            emit_field_set(&cg, in->op1, in->op2, val_ref, &t->ir);
            break;
        }
        case IR_RANGE_ASC:
        case IR_RANGE_DESC:
            emit_range(&cg, (IROp)in->op, in->op1, in->op2, i);
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

        if (is_inv && !cg.failed) {
            u32 op_size = cg_off(&cg) - op_start;
            if (op_size > 127) {
                /* Disp8 can't reach; falling back to disp32 here
                 * complicates patching with no existing v1 op
                 * exceeding 80 bytes -- bail conservatively. */
                cg.failed = true;
            } else {
                cg.base[skip_disp_off] = (u8)op_size;
            }
            /* Cache state across the skip is unknown -- see above. */
            cg_invalidate_xmm0(&cg);
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
