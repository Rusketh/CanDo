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
#include <math.h>     /* sqrt/fabs/floor/ceil for IR_CALL_F1 inlining */
#include "../object/object.h"   /* offsetof CdoObject for inline AREF */
#include "../object/value.h"    /* CDO_NUMBER tag + offsetof CdoValue */

/* External symbol for the side-exit helper (exported from jit.c). */
void cando_jit_replay_snapshot_for_mcode(struct CandoVM *vm,
                                          CandoTrace *t,
                                          TraceVal *vals,
                                          CandoValue *frame_slots,
                                          u32 snap_idx);
/* Phase 4.4 v1c: side-exit materialisation helper (jit.c).  Walks
 * t->sink_recs and writes a freshly-allocated heap object/array to
 * each recorded slot so post-trace bytecode reads the right value. */
void cando_jit_materialize_sunk_for_mcode(struct CandoVM *vm,
                                           CandoTrace *t,
                                           char *rbp_base,
                                           CandoValue *frame_slots);

#define CG_MAX_GUARDS     512    /* Phase 8.4: outer-loop traces with
                                    * unrolled inner can have many
                                    * inline INDEX_GET/SETs (2 guards
                                    * each).  nbody's outer trace 2
                                    * needs ~150 guards. */
#define CG_MAX_SUNK       16
#define CG_MAX_OBJ_FIELDS 8       /* per-sunk-object field cap */
#define CG_BUF_SIZE       32768u  /* eight pages; Phase 8.4 unrolled
                                    * outer-loop traces (e.g. nbody's
                                    * outer FOR with inner unrolled
                                    * via OP_FOR_INIT recording) need
                                    * 16-20KB.  Phase 8.2 inline array
                                    * access doubled per-op size from
                                    * the helper-call era (4096 was
                                    * tight even then). */

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

/* Phase 4.4 v1c: side-exit materialisation record.  Mirrors
 * CandoSinkRec (jit.h) -- copied to t->sink_recs after codegen
 * succeeds. */
typedef struct {
    u32 slot;
    i32 stack_off;
    u32 capacity;
    u8  is_array;
    u32 field_kref[CG_MAX_OBJ_FIELDS];
} CGSinkRec;

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
    /* Phase 4.4 v1c: per-trace side-exit materialisation list,
     * built as IR_SSTORE marked IRF_SUNK is processed.  Copied
     * to t->sink_recs after codegen finishes. */
    CGSinkRec sink_recs[CG_MAX_SUNK];
    u32       sink_count;

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

    /* Register pinning: keep loop-carried numeric scalars in
     * xmm6 .. xmm6+CG_MAX_PINNED-1 across iterations so the per-iter
     * SLOAD's slot read becomes a register copy and the SSTORE's slot
     * write becomes a register move + slot store rather than a memory-
     * to-memory hop.  Selected by a pre-pass that picks slots which
     * (a) have an IR_SLOAD with IRF_NUM_KNOWN, (b) are SSTORE'd as
     * IRT_NUM somewhere in the trace, (c) are not invariant.  Each
     * pinned slot is loaded + type-checked once in the prologue.
     *
     * pinned_slot_count is the number of pinned slots; pinned_slots[k]
     * is the (outer-frame-relative) slot index assigned to xmm(6+k);
     * pinned_first_load[k] is the IR_SLOAD's IRRef -- snapshot replay
     * uses vals[pinned_first_load[k]] as the pre-iter value, so we
     * keep it in sync via the SLOAD's "movsd vals[i], xmm".
     *
     * The xmm6+ range is caller-saved on SysV, so any C-helper call
     * inside the body would clobber them; the per-call save/restore
     * helpers below (cg_spill_pinned / cg_reload_pinned) wrap the
     * helper-call sites that the body emits. */
#define CG_MAX_PINNED 4u
    u32 pinned_slot_count;
    u32 pinned_slots      [CG_MAX_PINNED];
    u32 pinned_first_load [CG_MAX_PINNED];
    /* Codegen-time flag tracking whether the pinned xmm reg still
     * matches the value of pinned_first_load[k] (the IR_SLOAD's
     * result IRRef).  SSTORE to the same pinned slot writes the
     * "next iter's" value into the xmm reg, after which subsequent
     * arith / cmp ops in the SAME iter that reference the SLOAD's
     * IRRef must read from vals[] instead -- vals[first_load] is
     * still the iter-start value because nothing else writes there.
     * Reset at the start of each codegen pass; flipped on SSTORE. */
    bool pinned_overwritten[CG_MAX_PINNED];
    bool in_stub_emit;     /* once true, emit_call_rax stops spilling
                              pinned xmm regs around the call -- the
                              snapshot replay path has already flushed
                              to the slot and the trace is on its way
                              out. */
} CG;

/* Maps a slot to the xmm reg index assigned to it (6, 7, 8, 9), or 0
 * if unpinned.  Linear scan; CG_MAX_PINNED is small. */
static u32 cg_pin_xmm_for_slot(const CG *cg, u32 slot) {
    for (u32 k = 0; k < cg->pinned_slot_count; k++)
        if (cg->pinned_slots[k] == slot) return 6 + k;
    return 0;
}

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

/* Generic helpers that handle xmm0-xmm15 plus r14 (vals) and r15
 * (frame_slots) addressing.  Used by the register-pinning path so
 * the pinned xmm6+ regs are reachable.  Existing helpers above
 * (emit_movsd_xmm_vals etc.) hardcode REX.B=1 and assume xmm<=7;
 * keep them for the hot xmm0/xmm1 paths to avoid bloating those
 * call sites. */

/* movsd xmm_dst, xmm_src   -- F2 [REX] 0F 10 11 dst src
 * REX.R selects xmm_dst[8], REX.B selects xmm_src[8]. */
static void emit_movsd_xmm_xmm(CG *cg, u8 dst, u8 src) {
    cg_emit_u8(cg, 0xF2);
    u8 rex = 0;
    if (dst & 8) rex |= 0x44;        /* REX.R */
    if (src & 8) rex |= 0x41;        /* REX.B */
    if (rex) cg_emit_u8(cg, rex);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x10);
    cg_emit_u8(cg, (u8)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* movsd xmmN, [r14 + 8*idx]   for any N in 0..15. */
static void emit_movsd_xmmN_vals(CG *cg, u8 xmm, u32 idx) {
    cg_emit_u8(cg, 0xF2);
    /* Always REX.B=1 for r14 base; REX.R extends xmm. */
    u8 rex = 0x41;
    if (xmm & 8) rex |= 0x04;        /* REX.R */
    cg_emit_u8(cg, rex);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x10);
    cg_emit_u8(cg, (u8)(0x86 | ((xmm & 7) << 3)));
    cg_emit_u32(cg, idx * 8);
}

/* movsd [r14 + 8*idx], xmmN  for any N in 0..15. */
static void emit_movsd_vals_xmmN(CG *cg, u32 idx, u8 xmm) {
    cg_emit_u8(cg, 0xF2);
    u8 rex = 0x41;
    if (xmm & 8) rex |= 0x04;
    cg_emit_u8(cg, rex);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x11);
    cg_emit_u8(cg, (u8)(0x86 | ((xmm & 7) << 3)));
    cg_emit_u32(cg, idx * 8);
}

/* movsd xmmN, [r15 + 8*slot]  -- load a frame slot into a pinned xmm. */
static void emit_movsd_xmmN_slot(CG *cg, u8 xmm, u32 slot) {
    cg_emit_u8(cg, 0xF2);
    u8 rex = 0x41;                   /* REX.B for r15 */
    if (xmm & 8) rex |= 0x04;
    cg_emit_u8(cg, rex);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x10);
    cg_emit_u8(cg, (u8)(0x87 | ((xmm & 7) << 3)));
    cg_emit_u32(cg, slot * 8);
}

/* movsd [r15 + 8*slot], xmmN  -- store a pinned xmm back to a slot. */
static void emit_movsd_slot_xmmN(CG *cg, u32 slot, u8 xmm) {
    cg_emit_u8(cg, 0xF2);
    u8 rex = 0x41;
    if (xmm & 8) rex |= 0x04;
    cg_emit_u8(cg, rex);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x11);
    cg_emit_u8(cg, (u8)(0x87 | ((xmm & 7) << 3)));
    cg_emit_u32(cg, slot * 8);
}

/* mov rax, [r15 + 8*slot]  -- load the raw u64 NaN-box bits at a
 * frame slot.  Used by the prologue's per-pinned-slot type check. */
static void emit_mov_rax_r15_slot(CG *cg, u32 slot) {
    static const u8 b[] = { 0x49, 0x8B, 0x87 };  /* mov rax, [r15+disp32] */
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, slot * 8);
}

/* opXsd xmm0, xmmN where opXsd in {ADD=0x58, MUL=0x59, SUB=0x5C,
 * DIV=0x5E, ucomisd=0x2E (with 0x66 prefix instead of 0xF2)}. */
static void emit_arith_xmm0_xmmN(CG *cg, u8 op_byte, u8 xmm) {
    cg_emit_u8(cg, 0xF2);
    if (xmm & 8) cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, op_byte);
    cg_emit_u8(cg, (u8)(0xC0 | (xmm & 7)));
}

/* ucomisd xmm0, xmmN  -- 66 [REX] 0F 2E ModR/M.  Variant of the
 * existing emit_ucomisd_xmm0_xmm1 that takes a runtime xmm reg. */
static void emit_ucomisd_xmm0_xmmN(CG *cg, u8 xmm) {
    cg_emit_u8(cg, 0x66);
    if (xmm & 8) cg_emit_u8(cg, 0x41);
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x2E);
    cg_emit_u8(cg, (u8)(0xC0 | (xmm & 7)));
}

/* If `irref` is the IRRef of a pinned IR_SLOAD AND the pinned xmm
 * reg has not yet been overwritten by a same-iter SSTORE, return
 * the xmm reg holding its value (6, 7, ..).  Otherwise 0 -- the
 * caller falls back to vals[irref], which is the iter-start value
 * the SLOAD mirrored there. */
static u32 cg_pin_xmm_for_irref(const CG *cg, u32 irref) {
    for (u32 k = 0; k < cg->pinned_slot_count; k++) {
        if (cg->pinned_first_load[k] != irref) continue;
        if (cg->pinned_overwritten[k]) return 0;
        return 6 + k;
    }
    return 0;
}

/* Spill / reload pinned xmm regs around C-helper calls so caller-
 * saved xmm6+ survives the call.  We deliberately use frame_slots
 * (which the pinned IR_SSTORE keeps in sync, and which is the
 * snapshot's eventual destination anyway) rather than
 * vals[first_load]: the latter would be CLOBBERED if the helper
 * call follows an in-iter SSTORE, polluting the snapshot's pre-iter
 * value and double-counting the post-SSTORE value into downstream
 * consumers (e.g. `sum += i` would get sum += i+1 on the iter that
 * happens to take the GLOAD slow path). */
static void cg_spill_pinned(CG *cg) {
    for (u32 k = 0; k < cg->pinned_slot_count; k++)
        emit_movsd_slot_xmmN(cg, cg->pinned_slots[k], (u8)(6 + k));
}
static void cg_reload_pinned(CG *cg) {
    for (u32 k = 0; k < cg->pinned_slot_count; k++)
        emit_movsd_xmmN_slot(cg, (u8)(6 + k), cg->pinned_slots[k]);
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
/* lea rdx, [r14 + 8*idx]  -- replaced by lea_r8 in Phase 8.7's
 * cached gload helper; kept for any future 3-arg helpers.        */
__attribute__((unused))
static void emit_lea_rdx_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x49, 0x8D, 0x96 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* lea r8, [r14 + 8*idx]   -- 4D 8D 86 disp32.  Phase 8.7 5th
 * arg pointer for cando_jit_gload_cached_for_mcode (out-pointer). */
static void emit_lea_r8_vals(CG *cg, u32 idx) {
    static const u8 prefix[] = { 0x4D, 0x8D, 0x86 };
    cg_emit_bytes(cg, prefix, 3);
    cg_emit_u32(cg, idx * 8);
}
/* Phase 8.9 helpers for IR_LOOP's internal back-jump. */
/* inc DWORD [r12 + disp32]  -- 41 FF 84 24 disp32.  8 bytes. */
static void emit_inc_dword_r12_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x41, 0xFF, 0x84, 0x24 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u32(cg, (u32)disp);
}
/* mov r13b, 1              -- 41 B5 01.  3 bytes. */
static void emit_mov_r13b_one(CG *cg) {
    static const u8 b[] = { 0x41, 0xB5, 0x01 };
    cg_emit_bytes(cg, b, 3);
}
/* jmp rel32                -- E9 disp32.  5 bytes. */
static void emit_jmp_rel32(CG *cg, i32 disp) {
    cg_emit_u8(cg, 0xE9);
    cg_emit_u32(cg, (u32)disp);
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

/* JNE rel32 -- 0F 85 disp32.  Returns offset of the disp32. */
static u32 emit_jne_rel32_placeholder(CG *cg) {
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x85);
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    return disp_off;
}

/* JAE rel32 -- 0F 83 disp32.  Returns offset of the disp32. */
static u32 emit_jae_rel32_placeholder(CG *cg) {
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x83);
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    return disp_off;
}

/* mov ecx, imm32     -- B9 imm32. */
static void emit_mov_ecx_imm32(CG *cg, u32 imm) {
    cg_emit_u8(cg, 0xB9);
    cg_emit_u32(cg, imm);
}

/* mov ecx, [r12 + disp32]   -- 41 8B 8C 24 disp32  (32-bit load). */
static void emit_mov_ecx_r12_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x41, 0x8B, 0x8C, 0x24 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u32(cg, (u32)disp);
}

/* cmp ecx, [r12 + disp32]   -- 41 3B 8C 24 disp32  (32-bit cmp). */
static void emit_cmp_ecx_r12_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x41, 0x3B, 0x8C, 0x24 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u32(cg, (u32)disp);
}

/* mov rdx, [rbx + disp32]   -- 48 8B 93 disp32. */
static void emit_mov_rdx_rbx_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x8B, 0x93 };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}

/* cmp ecx, [rdx + disp32]   -- 3B 8A disp32  (32-bit cmp). */
static void emit_cmp_ecx_rdx_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x3B, 0x8A };
    cg_emit_bytes(cg, b, 2);
    cg_emit_u32(cg, (u32)disp);
}

/* mov rax, [rax + disp32]   -- 48 8B 80 disp32  (chained load). */
static void emit_mov_rax_rax_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x8B, 0x80 };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}

/* cmp BYTE [rax + disp32], imm8   -- 80 B8 disp32 imm8. */
static void emit_cmp_byte_rax_off_imm(CG *cg, i32 disp, u8 imm) {
    static const u8 b[] = { 0x80, 0xB8 };
    cg_emit_bytes(cg, b, 2);
    cg_emit_u32(cg, (u32)disp);
    cg_emit_u8(cg, imm);
}

/* ucomisd xmm0, xmm0   -- 66 0F 2E C0.  Sets PF=1 (and ZF=1) iff
 * xmm0 holds NaN.  Used as a NaN test in inline GSTORE so we can
 * dodge the helper's NaN canonicalisation only when xmm0 is a real
 * number; NaNs fall through to the helper which writes the canonical
 * positive qNaN bit pattern via cando_number(). */
static void emit_ucomisd_xmm0_xmm0(CG *cg) {
    static const u8 b[] = { 0x66, 0x0F, 0x2E, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}

/* JP rel32  -- 0F 8A disp32.  Branch on PF=1 (parity even); after
 * ucomisd this fires iff the comparison saw a NaN operand. */
static u32 emit_jp_rel32_placeholder(CG *cg) {
    cg_emit_u8(cg, 0x0F);
    cg_emit_u8(cg, 0x8A);
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
/* Phase 4.4 v1d: shadow-buffer plumbing for sunk allocations.
 *
 * mov rax, [r12 + disp32]   ; 49 8B 84 24 disp32  (load shadow ptr)
 * mov rcx, [rax + disp32]   ; 48 8B 88 disp32     (read shadow slot)
 * mov rcx, [rbp + disp32]   ; 48 8B 8D disp32     (read stack slot)
 * mov [rbp + disp32], rcx   ; 48 89 8D disp32     (write stack slot)
 * mov [rax + disp32], rcx   ; 48 89 88 disp32     (write shadow slot)
 * mov BYTE [r12 + disp32], 1 ; 41 C6 84 24 disp32 01  (set init flag)
 *
 * Always uses disp32 so callers don't worry about disp8/disp32
 * boundaries (CG_MAX_SUNK * CG_MAX_OBJ_FIELDS * 8 = 1024 bytes max,
 * which exceeds disp8 range for the negative rbp offsets).
 */
static void emit_mov_rax_r12_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x49, 0x8B, 0x84, 0x24 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u32(cg, (u32)disp);
}
static void emit_mov_rcx_rax_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x8B, 0x88 };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}
static void emit_mov_rcx_rbp_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x8B, 0x8D };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}
static void emit_mov_rbp_off_rcx(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x89, 0x8D };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}
static void emit_mov_rax_off_rcx(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x89, 0x88 };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}
static void emit_mov_r12_off_byte_imm(CG *cg, i32 disp, u8 imm) {
    static const u8 b[] = { 0x41, 0xC6, 0x84, 0x24 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u32(cg, (u32)disp);
    cg_emit_u8(cg, imm);
}
/* mov rdx, rbp   ; 48 89 EA  -- Phase 4.4 v1c: rbp_base arg for
 * the side-exit materialisation helper. */
static void emit_mov_rdx_rbp(CG *cg) {
    static const u8 b[] = { 0x48, 0x89, 0xEA }; cg_emit_bytes(cg, b, 3);
}
/* mov r13, r9    ; 4D 89 CD  -- stash snap_idx across the
 * materialise C call (r9 is caller-saved; r13 is callee-saved
 * AND free at the side-exit since LICM skip_invariant is no
 * longer needed once we're side-exiting). */
static void emit_mov_r13_r9(CG *cg) {
    static const u8 b[] = { 0x4D, 0x89, 0xCD }; cg_emit_bytes(cg, b, 3);
}
/* mov r9, r13    ; 4D 89 E9  -- restore snap_idx after the
 * materialise call so the replay helper sees it in r8 via
 * emit_mov_r8_r9. */
static void emit_mov_r9_r13(CG *cg) {
    static const u8 b[] = { 0x4D, 0x89, 0xE9 }; cg_emit_bytes(cg, b, 3);
}

/* call rax           -- FF D0.
 *
 * When register pinning is active and we're still emitting body code
 * (cg->in_stub_emit == false), spill the pinned xmm regs to vals[]
 * before the call and reload after.  xmm6+ are caller-saved on SysV,
 * so any helper call would otherwise clobber them.  Stub-area calls
 * happen AFTER the side-exit replay (which has already flushed to
 * the slot), so they don't need preserving. */
static void emit_call_rax(CG *cg) {
    bool wrap = cg->pinned_slot_count > 0 && !cg->in_stub_emit;
    if (wrap) cg_spill_pinned(cg);
    static const u8 b[] = { 0xFF, 0xD0 };
    cg_emit_bytes(cg, b, 2);
    if (wrap) cg_reload_pinned(cg);
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

    /* Phase 4.4 v1d: shadow buffer copy.  When sunk_total_bytes>0,
     * load t->sink_shadow into rax and copy each 8-byte slot into
     * the stack buffer.  This way the stack always reflects either
     * (a) the last LOOP_DONE'd iter's writes, or (b) zeros when
     * no iter has completed.  Materialise-on-side-exit reads
     * defined memory in either case (valgrind clean). */
    if (cg->sunk_total_bytes > 0) {
        emit_mov_rax_r12_off(cg, (i32)offsetof(struct CandoTrace,
                                                sink_shadow));
        u32 slots = cg->sunk_total_bytes / 8;
        for (u32 i = 0; i < slots; i++) {
            emit_mov_rcx_rax_off(cg, (i32)(8 * i));
            emit_mov_rbp_off_rcx(cg, -(i32)(56 + 8 * i));
        }
    }
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
 * checks (rax & NB_TAG_BITS_MASK) == NB_TAG_OBJECT.  Side-exits use
 * cg->cur_snap so any pinned SSTOREs since the last guard get
 * rolled back (matches the IR-interpreter at jit.c:IR_SLOAD).  An
 * earlier draft hardcoded snap=0, which left mid-iteration writes
 * unrestored on type-driven side-exits.
 *
 * When `num_known` is true (IRF_NUM_KNOWN; numeric-typed SLOAD whose
 * slot is also SSTORE'd as IRT_NUM elsewhere in the trace), the warm
 * path skips the guard: any iter past the first is preceded by an
 * SSTORE that wrote a numeric, so the slot is guaranteed numeric on
 * re-load.  Layout:
 *
 *     mov rax, [slot]
 *     mov [vals+i], rax           ; commit raw bits first
 *     test r13b, r13b
 *     jne  .skip                  ; warm: r13b=1, skip type check
 *     movabs rcx, NB_MASK
 *     and  rax, rcx
 *     cmp  rax, rcx
 *     je   side_exit              ; cold: validate and bail on mismatch
 *   .skip:
 *
 * The early store is safe because nothing past the guard reads
 * vals[i] until the next SLOAD's downstream use, and the snapshot
 * mechanism at side-exit reads vals[first_load].d (this same IRRef)
 * to restore the slot -- which already matches the slot's current
 * (loaded) value, so the snapshot replay is a no-op for this slot. */
static void emit_sload(CG *cg, u32 slot, u32 i, IRType slot_type,
                       bool num_known) {
    /* Pinned-register fast path: the slot's value lives in xmm6+
     * across iterations; the SLOAD just mirrors it to vals[i] so any
     * downstream non-pinned consumer (and snapshot replay) sees a
     * coherent value.  Type was already validated in the prologue's
     * cold-path load. */
    if (slot_type == IRT_NUM) {
        u32 xmm = cg_pin_xmm_for_slot(cg, slot);
        if (xmm) {
            emit_movsd_vals_xmmN(cg, i, (u8)xmm);
            cg_invalidate_xmm0(cg);
            return;
        }
    }

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
        cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
        cg->guards[cg->guard_count].stub_off    = 0;
        cg->guard_count++;
        /* Restore raw bits into rax (clobbered by AND) before store. */
        static const u8 mov_rax_rdx[] = { 0x48, 0x89, 0xD0 };
        cg_emit_bytes(cg, mov_rax_rdx, 3);
        emit_mov_vals_rax(cg, i);
        return;
    }

    if (num_known) {
        /* Commit raw bits to vals[i] BEFORE the guard so the warm-path
         * skip doesn't have to repeat the store -- and so the side-exit
         * stub's snapshot replay sees a coherent vals[first_load]. */
        emit_mov_vals_rax(cg, i);
        /* test r13b, r13b ; jne .skip -- 4 bytes incl. JNE disp8. */
        static const u8 test_jne[] = { 0x45, 0x84, 0xED, 0x75 };
        cg_emit_bytes(cg, test_jne, 4);
        u32 skip_disp_off = cg_off(cg);
        cg_emit_u8(cg, 0);              /* disp8 patched after guard */
        u32 guard_start = cg_off(cg);
        emit_movabs_rcx(cg, 0xFFF8000000000000ULL);
        emit_and_rax_rcx(cg);
        emit_cmp_rax_rcx(cg);
        if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
        cg->guards[cg->guard_count].je_disp_off = emit_je_rel32_placeholder(cg);
        cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
        cg->guards[cg->guard_count].stub_off    = 0;
        cg->guard_count++;
        u32 guard_end = cg_off(cg);
        /* Patch the disp8 to jump just past the guard sequence. */
        u32 disp = guard_end - (skip_disp_off + 1);
        if (disp > 127) { cg->failed = true; return; }
        cg->base[skip_disp_off] = (u8)disp;
        (void)guard_start;
        return;
    }

    /* Standard path: type-check before store so a bad type leaves
     * vals[i] untouched.  Side-exit at the JE.  After the check we
     * restore raw bits into rax from rdx (the AND clobbered them). */
    emit_movabs_rcx(cg, 0xFFF8000000000000ULL);       /* NB_MASK */
    emit_mov_rdx_rax(cg);
    emit_and_rax_rcx(cg);                             /* rax = v.u & MASK */
    emit_cmp_rax_rcx(cg);
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg->guards[cg->guard_count].je_disp_off = emit_je_rel32_placeholder(cg);
    cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;
    static const u8 mov_rax_rdx[] = { 0x48, 0x89, 0xD0 };  /* mov rax, rdx */
    cg_emit_bytes(cg, mov_rax_rdx, 3);
    emit_mov_vals_rax(cg, i);
}

/* IR_SSTORE slot, op2: vals[op2] -> frame_slots[slot].
 *
 * Pinned-slot path: update the pinned xmm reg AND the slot.  Updating
 * just the xmm reg would be tempting, but a side-exit at a guard
 * whose snapshot is empty (e.g. the loop-exit IR_LE failure, which
 * sees no preceding SSTOREs in the recorded iter) would skip
 * snapshot replay entirely and leave the slot at the pre-trace
 * value -- bytecode would then re-trip the back-edge and re-enter
 * the JIT forever.  Writing the slot on every SSTORE keeps the
 * snapshot mechanism robust and still buys us the SLOAD-side win
 * (the SLOAD's memory load + NaN-box guard are both gone for
 * pinned slots). */
static void emit_sstore(CG *cg, u32 slot, u32 op2) {
    u32 xmm = cg_pin_xmm_for_slot(cg, slot);
    if (xmm) {
        /* Pinned slot: only update the xmm reg.  The slot itself is
         * NOT written per iter -- the side-exit common stub writes
         * xmm to slot on bail (so an early guard that fires before
         * any SSTORE in this iter still restores the slot to its
         * iter-start value), and snapshot replay overwrites with
         * vals[first_load] when SSTOREs precede the failing guard. */
        emit_movsd_xmmN_vals(cg, (u8)xmm, op2);
        u32 k = (u32)xmm - 6u;
        if (k < CG_MAX_PINNED) cg->pinned_overwritten[k] = true;
        return;
    }
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
    /* Pinned-reg fast paths: when a or b is a pinned IR_SLOAD's IRRef,
     * the value lives in xmm6+ across iterations -- skip the
     * `movsd xmm0, [vals+]` load entirely.  RHS in a pinned reg
     * folds straight into the arith op as `addsd xmm0, xmmN` etc. */
    u32 a_xmm = cg_pin_xmm_for_irref(cg, a);
    u32 b_xmm = cg_pin_xmm_for_irref(cg, b);

    /* Stage LHS in xmm0. */
    if (a_xmm) {
        emit_movsd_xmm_xmm(cg, 0, (u8)a_xmm);
        cg_invalidate_xmm0(cg);    /* xmm0_holds tracking only knows
                                       about loads from vals */
    } else {
        emit_load_xmm0(cg, a);
    }

    if (a == b) {
        switch (op) {
        case IR_ADD: emit_addsd_self(cg); break;
        case IR_SUB: emit_subsd_self(cg); break;
        case IR_MUL: emit_mulsd_self(cg); break;
        case IR_DIV: emit_divsd_self(cg); break;
        default:     cg->failed = true; return;
        }
    } else if (b_xmm) {
        u8 op_byte;
        switch (op) {
        case IR_ADD: op_byte = 0x58; break;
        case IR_SUB: op_byte = 0x5C; break;
        case IR_MUL: op_byte = 0x59; break;
        case IR_DIV: op_byte = 0x5E; break;
        default:     cg->failed = true; return;
        }
        emit_arith_xmm0_xmmN(cg, op_byte, (u8)b_xmm);
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

/* Phase 8.2 inline emit primitives for the resolved-array fast
 * path.  Used when vals[op1] holds a CdoObject* (set by
 * emit_gload_arr or emit_hload_slot) rather than a NaN-boxed
 * handle.  Saves the call/ret overhead AND the per-access lock
 * acquire/release inside cdo_array_rawget_idx. */

/* mov rdi, [r14 + 8*idx]   -- already exists as emit_mov_rdi_vals. */

/* cmp esi, [rdi + disp32]  -- 3B B7 disp32. Bounds check vs items_len. */
static void emit_cmp_esi_rdi_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x3B, 0xB7 };
    cg_emit_bytes(cg, b, 2);
    cg_emit_u32(cg, (u32)disp);
}
/* mov rax, [rdi + disp32]  -- 48 8B 87 disp32.  Load arr->items. */
static void emit_mov_rax_rdi_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x48, 0x8B, 0x87 };
    cg_emit_bytes(cg, b, 3);
    cg_emit_u32(cg, (u32)disp);
}
/* shl rsi, 4   -- 48 C1 E6 04.  Multiply idx by sizeof(CdoValue)=16. */
static void emit_shl_rsi_4(CG *cg) {
    static const u8 b[] = { 0x48, 0xC1, 0xE6, 0x04 };
    cg_emit_bytes(cg, b, 4);
}
/* add rax, rsi  -- 48 01 F0.  rax += idx*16. */
static void emit_add_rax_rsi(CG *cg) {
    static const u8 b[] = { 0x48, 0x01, 0xF0 };
    cg_emit_bytes(cg, b, 3);
}
/* cmp BYTE [rax], imm8  -- 80 38 imm8.  Tag check at offset 0. */
static void emit_cmp_rax_byte_imm(CG *cg, u8 imm) {
    cg_emit_u8(cg, 0x80);
    cg_emit_u8(cg, 0x38);
    cg_emit_u8(cg, imm);
}
/* movsd xmm0, [rax + 8]  -- F2 0F 10 40 08.  Read items[idx].as.number. */
static void emit_movsd_xmm0_rax_off8(CG *cg, i8 disp) {
    static const u8 b[] = { 0xF2, 0x0F, 0x10, 0x40 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u8(cg, (u8)disp);
}
/* movsd [rax + 8], xmm0  -- F2 0F 11 40 08.  Write items[idx].as.number. */
static void emit_movsd_rax_off8_xmm0(CG *cg, i8 disp) {
    static const u8 b[] = { 0xF2, 0x0F, 0x11, 0x40 };
    cg_emit_bytes(cg, b, 4);
    cg_emit_u8(cg, (u8)disp);
}
/* mov BYTE [rax], imm8  -- C6 00 imm8.  Set tag at offset 0. */
static void emit_mov_rax_byte_imm(CG *cg, u8 imm) {
    cg_emit_u8(cg, 0xC6);
    cg_emit_u8(cg, 0x00);
    cg_emit_u8(cg, imm);
}

/* Inline IR_INDEX_GET / IR_AREF when vals[op1] holds CdoObject*.
 *   rdi  = vals[op1]                     (resolved arr ptr)
 *   esi  = (u32)(f64)vals[op2]           (idx)
 *   esi >= [rdi + items_len_off]   ?     side-exit (out of range)
 *   rax  = [rdi + items_off]             (items ptr)
 *   rax += esi * 16
 *   [rax + 0].tag != CDO_NUMBER    ?     side-exit (bad type)
 *   xmm0 = [rax + 8]                     (items[idx].as.number)
 *   vals[i] = xmm0
 */
static void emit_index_get_inline(CG *cg, u32 op1, u32 op2, u32 i) {
    i32 items_len_off = (i32)offsetof(struct CdoObject, items_len);
    i32 items_off     = (i32)offsetof(struct CdoObject, items);

    emit_mov_rdi_vals(cg, op1);
    emit_load_xmm0(cg, op2);
    emit_cvttsd2si_esi_xmm0(cg);
    emit_cmp_esi_rdi_off(cg, items_len_off);
    /* JAE = unsigned above-or-equal; treats negative idx as huge u32. */
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x83);   /* JAE rel32 */
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    cg->guards[cg->guard_count].je_disp_off = disp_off;
    cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;

    emit_mov_rax_rdi_off(cg, items_off);
    emit_shl_rsi_4(cg);
    emit_add_rax_rsi(cg);
    emit_cmp_rax_byte_imm(cg, CDO_NUMBER);
    /* JNE rel32 -- not a number, side-exit. */
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x85);   /* JNE rel32 */
    u32 disp_off2 = cg_off(cg);
    cg_emit_u32(cg, 0);
    cg->guards[cg->guard_count].je_disp_off = disp_off2;
    cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;

    emit_movsd_xmm0_rax_off8(cg, 8);
    emit_movsd_vals_xmm(cg, i, 0);
    cg->xmm0_holds = i;
}

/* Inline IR_INDEX_SET when vals[op1] holds CdoObject*.
 *   rdi  = vals[op1]                     (resolved arr ptr)
 *   esi  = (u32)(f64)vals[idx_op]        (idx)
 *   esi >= [rdi + items_len_off]   ?     side-exit
 *   rax  = [rdi + items_off]
 *   rax += esi * 16
 *   [rax + 0] = CDO_NUMBER (overwrite tag in case it changed)
 *   xmm0 = vals[val_op]
 *   [rax + 8] = xmm0
 *
 * Same atomic-free property: callers must ensure no other thread
 * mutates the array during a JIT'd region.  Same assumption as
 * everywhere else in the codegen. */
static void emit_index_set_inline(CG *cg, u32 op1, u32 idx_op, u32 val_op) {
    i32 items_len_off = (i32)offsetof(struct CdoObject, items_len);
    i32 items_off     = (i32)offsetof(struct CdoObject, items);

    emit_mov_rdi_vals(cg, op1);
    emit_load_xmm0(cg, idx_op);
    emit_cvttsd2si_esi_xmm0(cg);
    emit_cmp_esi_rdi_off(cg, items_len_off);
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    cg_emit_u8(cg, 0x0F); cg_emit_u8(cg, 0x83);
    u32 disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    cg->guards[cg->guard_count].je_disp_off = disp_off;
    cg->guards[cg->guard_count].snap_idx    = cg->cur_snap;
    cg->guards[cg->guard_count].stub_off    = 0;
    cg->guard_count++;

    emit_mov_rax_rdi_off(cg, items_off);
    emit_shl_rsi_4(cg);
    emit_add_rax_rsi(cg);
    emit_mov_rax_byte_imm(cg, CDO_NUMBER);
    emit_load_xmm0(cg, val_op);
    emit_movsd_rax_off8_xmm0(cg, 8);
    cg_invalidate_xmm0(cg);
}

/* Phase 8.2: classify op1's producer to decide whether vals[op1]
 * holds a CdoObject* (use inline path) or a CandoValue.u handle
 * (use the helper-call path with handle resolve). */
static bool ir_ref_holds_arr_ptr(const CandoTraceIR *ir, IRRef ref) {
    if (!ir || ref == 0) return false;
    if (ref >= ir->ir_count) return false;
    const IRIns *in = &ir->ir[ref];
    return (in->op == IR_GLOAD && in->type == IRT_OBJ) ||
           (in->op == IR_HLOAD_SLOT);
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
/* Phase 8.7: cached-entry-pointer variants.  vm/t/kidx/name/out
 * (5 args).  On warm cache the helper is a few-instruction
 * fast path -- no hash lookup, no string memcmp. */
extern int cando_jit_gload_cached_for_mcode(struct CandoVM *vm,
                                             CandoTrace *t, u32 kidx,
                                             struct CandoString *name,
                                             double *out);
extern int cando_jit_gstore_cached_for_mcode(struct CandoVM *vm,
                                              CandoTrace *t, u32 kidx,
                                              struct CandoString *name,
                                              double value);
/* Phase 8.2: resolve a global array to its CdoObject* once.
 * Returns NULL on bad type so the caller side-exits. */
extern void *cando_jit_gload_arr_for_mcode(struct CandoVM *vm,
                                            struct CandoString *name);

/* Phase 8.2: emit_gload_arr -- IR_GLOAD with IRT_OBJ.  Calls
 * cando_jit_gload_arr_for_mcode and stores the resolved CdoObject*
 * in vals[i].u.  Side-exit on NULL.  Subsequent IR_INDEX_GET on
 * this IRRef uses the pointer directly via the inline fast path
 * (no per-access lock, no handle resolve). */
static void emit_gload_arr(CG *cg, const CandoTraceIR *ir, IRRef name_ref, u32 i) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_movabs_rsi(cg, (u64)(uintptr_t)name);    /* arg2: name      */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gload_arr_for_mcode);
    emit_call_rax(cg);
    emit_test_rax_rax(cg);
    emit_je_to_stub(cg, cg->cur_snap);            /* NULL -> bad type */
    emit_mov_vals_rax(cg, i);                     /* vals[i] = obj_ptr */
    cg_invalidate_xmm0(cg);
}

static void emit_gload(CG *cg, const CandoTraceIR *ir, IRRef name_ref,
                       u32 i, bool inv) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    u32 kidx = IRREF_KIDX(name_ref);

    /* Skip the inline fast path when the op is loop-invariant: the
     * per-iter LICM `test r13b ; jne +disp8` skip caps op size at
     * 127 bytes, and the inline path is much larger.  Invariant ops
     * only execute on iter 1, so cache-warming gives no benefit -- a
     * single helper call is the right tradeoff. */
    if (inv) {
        emit_mov_rdi_rbx(cg);
        emit_mov_rsi_r12(cg);
        emit_mov_edx_imm(cg, kidx);
        emit_movabs_rcx(cg, (u64)(uintptr_t)name);
        emit_lea_r8_vals(cg, i);
        emit_movabs_rax(cg,
            (u64)(uintptr_t)&cando_jit_gload_cached_for_mcode);
        emit_call_rax(cg);
        emit_test_eax_eax(cg);
        emit_jne_to_stub(cg, cg->cur_snap);
        cg_invalidate_xmm0(cg);
        return;
    }

    /* Inline warm-path fast lookup: skip the helper call when the
     * trace's per-kidx entry-pointer cache is hot AND globals_version
     * matches.  The slow-path helper still ships untouched at the
     * end so cold cache and bad-type all bail through it correctly.
     *
     * Layout (rax/rcx/rdx are scratch; r12 = trace, rbx = vm,
     * r14 = vals):
     *
     *   mov  ecx, kidx
     *   cmp  ecx, [r12 + cap_off]
     *   jae  slow                        ; kidx out of cache range
     *   mov  rax, [r12 + cache_off]
     *   test rax, rax
     *   jz   slow                        ; cache table not allocated
     *   mov  rax, [rax + 8*kidx]         ; entry pointer
     *   test rax, rax
     *   jz   slow                        ; entry not yet cached
     *   mov  ecx, [r12 + ver_seen_off]
     *   mov  rdx, [rbx + globals_off]
     *   cmp  ecx, [rdx + ver_off]
     *   jne  slow                        ; rehash since cache populated
     *   mov  rax, [rax + value_off]      ; entry->value.u
     *   mov  [r14 + 8*i], rax
     *   movabs rcx, NB_MASK
     *   and  rax, rcx
     *   cmp  rax, rcx
     *   je   slow                        ; non-numeric -> let helper bail
     *   jmp  done
     * slow:
     *   <existing 5-arg helper call>
     * done:
     */
    u32 slow_jumps[5];
    u32 n_slow = 0;

    emit_mov_ecx_imm32(cg, kidx);
    emit_cmp_ecx_r12_off(cg,
        (i32)offsetof(struct CandoTrace, gload_entry_cache_cap));
    slow_jumps[n_slow++] = emit_jae_rel32_placeholder(cg);

    emit_mov_rax_r12_off(cg,
        (i32)offsetof(struct CandoTrace, gload_entry_cache));
    emit_test_rax_rax(cg);
    slow_jumps[n_slow++] = emit_je_rel32_placeholder(cg);

    /* 8 * kidx fits in disp32 for any sane kidx (const-pool indices
     * stay well under 2^28). */
    emit_mov_rax_rax_off(cg, (i32)(8u * kidx));
    emit_test_rax_rax(cg);
    slow_jumps[n_slow++] = emit_je_rel32_placeholder(cg);

    emit_mov_ecx_r12_off(cg,
        (i32)offsetof(struct CandoTrace, globals_version_seen));
    emit_mov_rdx_rbx_off(cg, (i32)offsetof(struct CandoVM, globals));
    emit_cmp_ecx_rdx_off(cg,
        (i32)offsetof(struct CandoGlobalEnv, version));
    slow_jumps[n_slow++] = emit_jne_rel32_placeholder(cg);

    /* Cache + version OK.  Load value, commit, then verify type. */
    emit_mov_rax_rax_off(cg,
        (i32)offsetof(struct CandoGlobalEntry, value));
    emit_mov_vals_rax(cg, i);
    emit_movabs_rcx(cg, 0xFFF8000000000000ULL);   /* NB_MASK */
    emit_and_rax_rcx(cg);
    emit_cmp_rax_rcx(cg);
    slow_jumps[n_slow++] = emit_je_rel32_placeholder(cg);

    u32 jmp_done = emit_jmp_rel32_placeholder(cg);

    /* Slow path: existing 5-arg helper (vm, t, kidx, name, &vals[i]). */
    u32 slow_off = cg_off(cg);
    for (u32 k = 0; k < n_slow; k++)
        cg_patch_rel32(cg, slow_jumps[k], slow_off);

    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_mov_rsi_r12(cg);                         /* arg2: t         */
    emit_mov_edx_imm(cg, kidx);                   /* arg3: kidx      */
    emit_movabs_rcx(cg, (u64)(uintptr_t)name);    /* arg4: name      */
    emit_lea_r8_vals(cg, i);                      /* arg5: &vals[i]  */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gload_cached_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);

    u32 done_off = cg_off(cg);
    cg_patch_rel32(cg, jmp_done, done_off);
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

/* IR_AREF formerly used cando_jit_aref_for_mcode -- replaced by
 * emit_index_get_inline (the path is identical: ptr + numeric idx).
 * The helper still ships in jit.c for the IR-interpreter path. */

/* mov eax, [rdi + disp32]   -- 8B 87 disp32.  Load 32-bit field
 * (e.g. items_len) from a CdoObject pointed to by rdi. */
static void emit_mov_eax_rdi_off(CG *cg, i32 disp) {
    static const u8 b[] = { 0x8B, 0x87 };
    cg_emit_bytes(cg, b, 2);
    cg_emit_u32(cg, (u32)disp);
}
/* IR_HLEN: vals[i] = (f64)((CdoObject*)vals[op1])->items_len.
 * Phase 8.3: open-coded load + i32->f64 conversion.
 *
 * op1's vals[] may hold either:
 *   - a CdoObject* (when produced by IR_HLOAD_SLOT or IR_HLOAD, or
 *     by IR_GLOAD with IRT_OBJ after Phase 8.2)
 *   - a CandoValue.u handle (when produced by IR_RANGE_*, IR_NEW_*).
 *
 * We dispatch on the producer's IRType via the trace IR.  For
 * handles, call cando_bridge_resolve to convert; the helper
 * returns NULL on bad-type (we side-exit).  Within the same
 * trace, the kind is stable so no per-call kind check is
 * needed once IR_HLEN fired once. */
static void emit_hlen(CG *cg, const CandoTraceIR *ir, u32 op1, u32 i) {
    i32 items_len_off = (i32)offsetof(struct CdoObject, items_len);
    bool is_ptr = false;
    if (op1 < ir->ir_count) {
        const IRIns *src = &ir->ir[op1];
        is_ptr = (src->type == IRT_PTR);
    }
    if (is_ptr) {
        emit_mov_rdi_vals(cg, op1);
    } else {
        /* Handle source: resolve via cando_bridge_resolve(vm, idx). */
        emit_mov_rdi_rbx(cg);                     /* arg1: vm */
        emit_load_xmm0(cg, op1);                  /* xmm0 = handle f64 lane */
        /* Reinterpret the u64 as the handle: the helper takes a
         * CandoHandle which is a u32 index.  We need to extract
         * that from the boxed value.  Simpler: call a thin
         * wrapper that takes the raw u64 and returns the obj ptr
         * (or NULL on bad type). */
        emit_mov_rsi_vals(cg, op1);               /* arg2: arr_u u64 */
        extern void *cando_jit_resolve_arr_for_mcode(struct CandoVM *vm,
                                                      u64 arr_u);
        emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_resolve_arr_for_mcode);
        emit_call_rax(cg);
        emit_test_rax_rax(cg);
        emit_je_to_stub(cg, cg->cur_snap);
        /* rax now holds CdoObject*; copy to rdi for the next read. */
        static const u8 mov_rdi_rax[] = { 0x48, 0x89, 0xC7 };
        cg_emit_bytes(cg, mov_rdi_rax, 3);
    }
    emit_mov_eax_rdi_off(cg, items_len_off);      /* eax = items_len */
    emit_cvtsi2sd_xmm0_eax(cg);                   /* xmm0 = (f64)eax */
    emit_movsd_vals_xmm(cg, i, 0);                /* vals[i] = xmm0 */
    cg->xmm0_holds = i;
}

static void emit_gstore(CG *cg, const CandoTraceIR *ir, IRRef name_ref,
                        u32 op2, bool inv) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    CandoString *name = cando_as_string(cv);
    u32 kidx = IRREF_KIDX(name_ref);

    if (inv) {
        /* Same rationale as emit_gload's invariant branch -- the
         * 80+ byte inline doesn't fit in the LICM-skip disp8 budget,
         * and a value that only gets written once doesn't need the
         * inline anyway. */
        emit_mov_rdi_rbx(cg);
        emit_mov_rsi_r12(cg);
        emit_mov_edx_imm(cg, kidx);
        emit_movabs_rcx(cg, (u64)(uintptr_t)name);
        emit_load_xmm0(cg, op2);
        emit_movabs_rax(cg,
            (u64)(uintptr_t)&cando_jit_gstore_cached_for_mcode);
        emit_call_rax(cg);
        emit_test_eax_eax(cg);
        emit_jne_to_stub(cg, cg->cur_snap);
        cg_invalidate_xmm0(cg);
        return;
    }

    /* Inline warm-path fast write: same cache-pointer handshake as
     * emit_gload, plus an is_const guard and a NaN bail (so a NaN
     * result from arith doesn't alias the boxed-value tag space).
     * On any miss / version mismatch / const-protected entry / NaN
     * value, fall through to the existing 5-arg helper which handles
     * all the boxing edge cases.
     *
     * xmm0 holds the value (loaded from vals[op2]).  Layout is:
     *
     *   <cache lookup, identical to emit_gload>      ; rax = entry
     *   cmp BYTE [rax + is_const_off], 0
     *   jne slow                                     ; const-protected
     *   ucomisd xmm0, xmm0
     *   jp  slow                                     ; NaN -> helper
     *   movsd [rax + value_off], xmm0
     *   jmp done
     * slow:
     *   <existing 5-arg helper>
     * done:
     */
    emit_load_xmm0(cg, op2);                      /* xmm0 = value     */

    u32 slow_jumps[7];
    u32 n_slow = 0;

    emit_mov_ecx_imm32(cg, kidx);
    emit_cmp_ecx_r12_off(cg,
        (i32)offsetof(struct CandoTrace, gload_entry_cache_cap));
    slow_jumps[n_slow++] = emit_jae_rel32_placeholder(cg);

    emit_mov_rax_r12_off(cg,
        (i32)offsetof(struct CandoTrace, gload_entry_cache));
    emit_test_rax_rax(cg);
    slow_jumps[n_slow++] = emit_je_rel32_placeholder(cg);

    emit_mov_rax_rax_off(cg, (i32)(8u * kidx));
    emit_test_rax_rax(cg);
    slow_jumps[n_slow++] = emit_je_rel32_placeholder(cg);

    emit_mov_ecx_r12_off(cg,
        (i32)offsetof(struct CandoTrace, globals_version_seen));
    emit_mov_rdx_rbx_off(cg, (i32)offsetof(struct CandoVM, globals));
    emit_cmp_ecx_rdx_off(cg,
        (i32)offsetof(struct CandoGlobalEnv, version));
    slow_jumps[n_slow++] = emit_jne_rel32_placeholder(cg);

    /* is_const guard.  CandoGlobalEntry.is_const is a bool (1 byte)
     * placed after key + value; a non-zero byte means write-protected. */
    emit_cmp_byte_rax_off_imm(cg,
        (i32)offsetof(struct CandoGlobalEntry, is_const), 0);
    slow_jumps[n_slow++] = emit_jne_rel32_placeholder(cg);

    /* NaN bail -- the helper canonicalises NaNs to positive qNaN to
     * keep them out of the NaN-box tag space; we don't want to write
     * a raw negative-NaN that would alias a boxed value. */
    emit_ucomisd_xmm0_xmm0(cg);
    slow_jumps[n_slow++] = emit_jp_rel32_placeholder(cg);

    /* offsetof(CandoGlobalEntry, value) is 8 in the current layout
     * (key=8, then value=8); use the existing disp8 movsd helper.
     * Static_assert below catches a future re-layout. */
    _Static_assert(offsetof(struct CandoGlobalEntry, value) == 8,
                   "GSTORE inline assumes CandoGlobalEntry.value is at offset 8");
    emit_movsd_rax_off8_xmm0(cg, 8);

    u32 jmp_done = emit_jmp_rel32_placeholder(cg);

    /* Slow path: existing 5-arg helper (vm, t, kidx, name, value). */
    u32 slow_off = cg_off(cg);
    for (u32 k = 0; k < n_slow; k++)
        cg_patch_rel32(cg, slow_jumps[k], slow_off);

    emit_mov_rdi_rbx(cg);                         /* arg1: vm        */
    emit_mov_rsi_r12(cg);                         /* arg2: t         */
    emit_mov_edx_imm(cg, kidx);                   /* arg3: kidx      */
    emit_movabs_rcx(cg, (u64)(uintptr_t)name);    /* arg4: name      */
    /* xmm0 still holds the value -- no reload needed before the call. */
    emit_movabs_rax(cg, (u64)(uintptr_t)&cando_jit_gstore_cached_for_mcode);
    emit_call_rax(cg);
    emit_test_eax_eax(cg);
    emit_jne_to_stub(cg, cg->cur_snap);

    u32 done_off = cg_off(cg);
    cg_patch_rel32(cg, jmp_done, done_off);
    cg_invalidate_xmm0(cg);                       /* call may have clobbered */
}

/* sqrtsd xmm0, xmm0   ; F2 0F 51 C0   (4 bytes; in-place sqrt of xmm0). */
static void emit_sqrtsd_xmm0(CG *cg) {
    static const u8 b[] = { 0xF2, 0x0F, 0x51, 0xC0 };
    cg_emit_bytes(cg, b, 4);
}

/* IR_CALL_F1: vals[i] = fast_native(vals[op2]).  op1 is the index
 * into vm->fast_natives_f1[].  We resolve the function pointer at
 * codegen time and embed it as an immediate -- registrations are
 * write-once at startup so the pointer is stable for the trace's
 * lifetime.  SysV ABI: f64 arg in XMM0, result in XMM0.  Stack is
 * 16-aligned at this point (5 callee-saved pushes from the prologue
 * land RSP at aligned), so `call rax` to the helper is well-formed.
 *
 * Phase 8.2: detect well-known libm functions whose semantics map
 * to a single SSE2 instruction and inline directly -- avoids the
 * call/ret pair plus the wrapper's argument-passing overhead.
 * Currently: sqrt -> sqrtsd.  Hot in nbody (~21 calls per inner
 * iter, 9919 trace iters total). */
static void emit_call_f1(CG *cg, u32 native_idx, u32 op2, u32 i) {
    if (!cg->vm || native_idx >= cg->vm->fast_natives_f1_cap ||
        cg->vm->fast_natives_f1[native_idx] == NULL) {
        cg->failed = true;
        return;
    }
    CandoFastFn1 fn = cg->vm->fast_natives_f1[native_idx];
    emit_load_xmm0(cg, op2);                 /* xmm0 = vals[op2]     */
    if (fn == (CandoFastFn1)sqrt) {
        emit_sqrtsd_xmm0(cg);
    } else {
        emit_movabs_rax(cg, (u64)(uintptr_t)fn);
        emit_call_rax(cg);
    }
    emit_movsd_vals_xmm(cg, i, 0);           /* vals[i] = xmm0       */
    cg->xmm0_holds = i;
}

/* Comparisons: vals[a] CMP vals[b] -> vals[i] as 1.0 or 0.0.
 * Mirrors the IR-interp's "(a CMP b) ? 1.0 : 0.0" semantics.  We
 * trust no NaN inputs in v1 (numeric IR by construction). */
static void emit_compare(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    /* Mirrors emit_arith's pinned-reg shortcuts: skip the LHS vals[]
     * load when a is pinned, and feed RHS as `ucomisd xmm0, xmmN`
     * when b is pinned. */
    u32 a_xmm = cg_pin_xmm_for_irref(cg, a);
    u32 b_xmm = cg_pin_xmm_for_irref(cg, b);
    if (a_xmm) {
        emit_movsd_xmm_xmm(cg, 0, (u8)a_xmm);
        cg_invalidate_xmm0(cg);
    } else {
        emit_load_xmm0(cg, a);
    }
    if (b_xmm) {
        emit_ucomisd_xmm0_xmmN(cg, (u8)b_xmm);
    } else {
        emit_movsd_xmm_vals(cg, 1, b);
        emit_ucomisd_xmm0_xmm1(cg);
    }
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

/* Register-pinning pre-pass: select up to CG_MAX_PINNED slots that
 * are good candidates for keeping in callee-managed xmm regs across
 * iterations.  A slot qualifies when (a) the trace contains an
 * IR_SLOAD of it tagged IRF_NUM_KNOWN (i.e. an SSTORE elsewhere
 * proves the slot is numeric on subsequent iters), (b) the slot
 * isn't IRF_INVARIANT (which would already be skipped on warm).
 *
 * For each pinned slot we record the IRRef of the FIRST IR_SLOAD --
 * that's the IRRef snapshot replay reads from vals[] to restore the
 * pre-iter slot value, so the SLOAD codegen keeps writing vals[]
 * from the pinned register to keep snapshots correct.
 *
 * Picks the first qualifying slots in IR-emission order; this favours
 * loop counters / FOR_RANGE state and accumulators bound first by
 * the recorder, which is exactly the high-traffic data. */
static void cg_assign_pinned_slots(CG *cg, const CandoTraceIR *ir) {
    cg->pinned_slot_count = 0;
    if (ir->ir_count <= 1) return;
    for (u32 i = 1; i < ir->ir_count &&
                    cg->pinned_slot_count < CG_MAX_PINNED; i++) {
        const IRIns *in = &ir->ir[i];
        if (in->op != IR_SLOAD) continue;
        if (in->type != IRT_NUM) continue;
        if (!(in->flags & IRF_NUM_KNOWN)) continue;
        if (in->flags & IRF_INVARIANT) continue;
        u32 slot = in->op1;
        bool dup = false;
        for (u32 k = 0; k < cg->pinned_slot_count; k++)
            if (cg->pinned_slots[k] == slot) { dup = true; break; }
        if (dup) continue;
        cg->pinned_slots     [cg->pinned_slot_count] = slot;
        cg->pinned_first_load[cg->pinned_slot_count] = i;
        cg->pinned_slot_count++;
    }
}

/* For the codegen body: load the pinned slot into its xmm reg.
 * Emitted at the START of each iter ONLY on the first iter via the
 * standard skip_invariant gate -- on iter 2+ the previous iter's
 * SSTORE has left the right value in the xmm reg, so we just skip
 * the reload.  Type checks are folded in -- a non-numeric value
 * side-exits to the trace's first guard's snapshot.
 *
 * Layout per pinned slot, gated on (skip_invariant == false):
 *
 *   mov   rax, [r15 + 8*slot]     ; load NaN-box bits
 *   movq  xmmN, rax               ; copy bits -> xmm
 *   ; type check on rax (cold path)
 *   movabs rcx, NB_MASK
 *   and   rax, rcx
 *   cmp   rax, rcx
 *   je    side_exit               ; not a number -> bail
 *
 * The xmm reg also caches in vals[first_load] (so snapshot replay
 * sees a coherent pre-iter value).  We do that with a single movsd
 * after the check, which doubles as the "vals[] mirror". */
static void emit_pinned_prologue(CG *cg) {
    if (cg->pinned_slot_count == 0) return;
    /* Gate on skip_invariant -- iter 2+ trusts the prior SSTORE. */
    static const u8 test_jne[] = { 0x45, 0x84, 0xED, 0x0F, 0x85 };
    cg_emit_bytes(cg, test_jne, 5);
    u32 skip_disp_off = cg_off(cg);
    cg_emit_u32(cg, 0);
    u32 cold_start = cg_off(cg);
    for (u32 k = 0; k < cg->pinned_slot_count; k++) {
        u8  xmm  = (u8)(6 + k);
        u32 slot = cg->pinned_slots[k];
        emit_mov_rax_r15_slot(cg, slot);
        /* movq xmm, rax -- 66 [REX] 0F 6E ModR/M; using REX.W for
         * 64-bit GPR transfer.  REX.R for xmm>=8 (we don't reach
         * here with CG_MAX_PINNED=4). */
        cg_emit_u8(cg, 0x66);
        u8 rex = 0x48;                          /* REX.W */
        if (xmm & 8) rex |= 0x04;
        cg_emit_u8(cg, rex);
        cg_emit_u8(cg, 0x0F);
        cg_emit_u8(cg, 0x6E);
        cg_emit_u8(cg, (u8)(0xC0 | ((xmm & 7) << 3) | 0));   /* rax = 0 */
        /* Type check: (rax & NB_MASK) == NB_MASK -> not a number. */
        emit_movabs_rcx(cg, 0xFFF8000000000000ULL);
        emit_and_rax_rcx(cg);
        emit_cmp_rax_rcx(cg);
        if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
        cg->guards[cg->guard_count].je_disp_off = emit_je_rel32_placeholder(cg);
        cg->guards[cg->guard_count].snap_idx    = 0;   /* trace head */
        cg->guards[cg->guard_count].stub_off    = 0;
        cg->guard_count++;
        /* Mirror to vals[first_load] so snapshot replay restores the
         * slot to its pre-iter value on later guard fails. */
        emit_movsd_vals_xmmN(cg, cg->pinned_first_load[k], xmm);
    }
    /* Patch the JNE to land here. */
    u32 here = cg_off(cg);
    i32 disp = (i32)here - (i32)(skip_disp_off + 4);
    cg->base[skip_disp_off + 0] = (u8)(disp & 0xFF);
    cg->base[skip_disp_off + 1] = (u8)((disp >> 8) & 0xFF);
    cg->base[skip_disp_off + 2] = (u8)((disp >> 16) & 0xFF);
    cg->base[skip_disp_off + 3] = (u8)((disp >> 24) & 0xFF);
    (void)cold_start;
}

/* Function-trace codegen.  Different mcode signature from the loop
 * trace path:
 *
 *   int func_mcode(CandoVM *vm, CandoTrace *t, double arg0)
 *
 * vm in rdi, t in rsi, arg0 in xmm0.  Returns 0 in eax and the
 * function value in xmm0 on success; returns 1 in eax (xmm0
 * undefined) on any guard failure -- the OP_CALL dispatcher then
 * falls back to pushing a real VM frame and running the function
 * via the bytecode interpreter.
 *
 * Layout: prologue stack-allocates a vals[] scratch area and a
 * fake frame_slots area, stores arg0 to fake_slots[1] (slot 0 is
 * reserved for the function-value sentinel, parameters start at 1
 * per CanDo's call-frame convention).  Body codegen uses the
 * existing per-IR emitters; r14 / r15 point at the stack-local
 * vals[] / fake_slots[] so SLOAD/SSTORE/arith all work unchanged.
 *
 * IR_RETURN: load vals[op1] -> xmm0, xor eax,eax, jump to common
 * epilogue.  IR_REC_CALL: spill, prepare arg, `call rel32 0` (back
 * to start of this very mcode), check eax, propagate failure or
 * read result from xmm0.
 *
 * No internal loop, no LICM -- the body executes once per call.
 * Pinning is disabled (function-trace bodies don't have a loop edge
 * that would benefit). */
static bool cando_jit_codegen_func_trace(struct CandoVM *vm, CandoTrace *t) {
    if (t->ir.ir_count == 0) return false;
    if (!cando_mcode_alloc(&t->mcode, CG_BUF_SIZE)) return false;

    CG cg = (CG){0};
    cg.base = t->mcode.base;
    cg.cur  = t->mcode.base;
    cg.end  = t->mcode.base + t->mcode.size;
    cg.vm   = vm;
    /* Pinning intentionally off for function traces. */

    /* Determine sizes for vals[] and fake frame_slots based on the
     * highest IRRef and slot accessed. */
    u32 vals_count = t->ir.ir_count;     /* one entry per IR_? */
    u32 max_slot   = 4;                   /* room for slot 0..3 minimum */
    for (u32 i = 1; i < t->ir.ir_count; i++) {
        const IRIns *in = &t->ir.ir[i];
        if ((in->op == IR_SLOAD || in->op == IR_SSTORE) &&
            in->op1 + 1 > max_slot) {
            max_slot = in->op1 + 1;
        }
    }
    u32 vals_bytes  = vals_count * 8;
    u32 slots_bytes = max_slot   * 8;
    /* 16-byte align the total stack alloc. */
    u32 alloc_total = vals_bytes + slots_bytes + 8;
    if (alloc_total % 16 != 0) alloc_total = ((alloc_total + 15) / 16) * 16;

    /* Prologue. */
    static const u8 fixed1[] = {
        0x55,                              /* push rbp           */
        0x48, 0x89, 0xE5,                  /* mov rbp, rsp       */
        0x53,                              /* push rbx           */
        0x41, 0x54,                        /* push r12           */
        0x41, 0x55,                        /* push r13           */
        0x41, 0x56,                        /* push r14           */
        0x41, 0x57,                        /* push r15           */
    };
    cg_emit_bytes(&cg, fixed1, sizeof(fixed1));
    /* sub rsp, alloc_total. */
    if (alloc_total < 0x80) {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x83);
        cg_emit_u8(&cg, 0xEC); cg_emit_u8(&cg, (u8)alloc_total);
    } else {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x81);
        cg_emit_u8(&cg, 0xEC); cg_emit_u32(&cg, alloc_total);
    }
    /* mov rbx, rdi  (vm)            -- 48 89 FB
     * mov r12, rsi  (t)             -- 49 89 F4
     * mov r13b, 0   (skip_invariant unused but compat) -- 41 B5 00
     *   Reuse emit_mov_r13b_one but set 0 -- inline 41 B5 00
     * lea r14, [rsp]                -- 4C 8D 34 24
     * lea r15, [rsp + vals_bytes]   -- 4D 8D BC 24 disp32
     * movsd [r15 + 8], xmm0         -- store arg0 to fake_slots[1] */
    static const u8 setup[] = {
        0x48, 0x89, 0xFB,                  /* mov rbx, rdi  */
        0x49, 0x89, 0xF4,                  /* mov r12, rsi  */
        0x41, 0xB5, 0x00,                  /* mov r13b, 0   */
        0x4C, 0x8D, 0x34, 0x24,            /* lea r14, [rsp] */
    };
    cg_emit_bytes(&cg, setup, sizeof(setup));
    /* lea r15, [rsp + vals_bytes] -- 4D 8D BC 24 disp32 */
    static const u8 lea_r15[] = { 0x4D, 0x8D, 0xBC, 0x24 };
    cg_emit_bytes(&cg, lea_r15, 4);
    cg_emit_u32(&cg, vals_bytes);
    /* movsd [r15 + 8], xmm0  -- F2 41 0F 11 87 disp32  (offset 8 for slot 1) */
    static const u8 store_arg[] = { 0xF2, 0x41, 0x0F, 0x11, 0x87,
                                     0x08, 0x00, 0x00, 0x00 };
    cg_emit_bytes(&cg, store_arg, sizeof(store_arg));

    u32 mcode_entry_off = 0;   /* IR_REC_CALL targets the start (0) */

    /* Body emit. */
    for (u32 i = 1; i < t->ir.ir_count && !cg.failed; i++) {
        const IRIns *in = &t->ir.ir[i];

        switch (in->op) {
        case IR_NOP:
            break;
        case IR_KNUM:
            emit_knum(&cg, &t->ir, in->op1, i);
            break;
        case IR_KBOOL:
            emit_kbool(&cg, in->op1, i);
            break;
        case IR_SLOAD:
            if (in->type != IRT_NUM && in->type != IRT_OBJ) {
                cg.failed = true; break;
            }
            emit_sload(&cg, in->op1, i, (IRType)in->type, false);
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
        case IR_EQ: case IR_NEQ: case IR_LT: case IR_LE:
        case IR_GT: case IR_GE:
            emit_compare(&cg, (IROp)in->op, in->op1, in->op2, i);
            break;
        case IR_GUARD_NUM:
            break;
        case IR_GUARD_TRUE: case IR_GUARD_FALSE:
            emit_guard_bool(&cg, (IROp)in->op, in->op1, 0);
            break;
        case IR_REC_CALL: {
            /* Compute arg into xmm0, then emit a near `call rel32 0`
             * that jumps to the start of this same mcode buffer. */
            emit_load_xmm0(&cg, in->op1);
            /* mov rdi, rbx ; mov rsi, r12 */
            emit_mov_rdi_rbx(&cg);
            emit_mov_rsi_r12(&cg);
            /* call rel32  -- E8 disp32 ; disp = mcode_entry - (here+5) */
            cg_emit_u8(&cg, 0xE8);
            u32 disp_off = cg_off(&cg);
            i32 here_after = (i32)(disp_off + 4);
            i32 disp = (i32)mcode_entry_off - here_after;
            cg_emit_u32(&cg, (u32)disp);
            /* test eax, eax ; jne side_exit */
            emit_test_eax_eax(&cg);
            if (cg.guard_count >= CG_MAX_GUARDS) { cg.failed = true; break; }
            cg.guards[cg.guard_count].je_disp_off = emit_jne_rel32_placeholder(&cg);
            cg.guards[cg.guard_count].snap_idx    = 0;
            cg.guards[cg.guard_count].stub_off    = 0;
            cg.guard_count++;
            /* xmm0 has the result -- mirror to vals[i]. */
            emit_movsd_vals_xmm(&cg, i, 0);
            cg.xmm0_holds = i;
            break;
        }
        case IR_RETURN: {
            /* Load value to xmm0, set eax=0, jump to common epilogue. */
            emit_load_xmm0(&cg, in->op1);
            emit_xor_eax_eax(&cg);
            /* jmp to common epilogue (we'll patch this disp32 after
             * the body since we don't know the epilogue location yet).
             * Use the guard table mechanism with a sentinel: we record
             * a "return jump" with snap_idx=0xFFFE so the post-pass
             * patches it to the success epilogue rather than a stub. */
            if (cg.guard_count >= CG_MAX_GUARDS) { cg.failed = true; break; }
            cg_emit_u8(&cg, 0xE9);   /* jmp rel32 */
            cg.guards[cg.guard_count].je_disp_off = cg_off(&cg);
            cg.guards[cg.guard_count].snap_idx    = 0xFFFE;
            cg.guards[cg.guard_count].stub_off    = 0;
            cg_emit_u32(&cg, 0);
            cg.guard_count++;
            break;
        }
        default:
            cg.failed = true;
            break;
        }
    }

    if (cg.failed) {
        cando_mcode_free(&t->mcode);
        return false;
    }

    /* Common success epilogue: eax already 0 and xmm0 holds value
     * before each IR_RETURN's jump lands here. */
    u32 success_off = cg_off(&cg);
    /* add rsp, alloc_total */
    if (alloc_total < 0x80) {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x83);
        cg_emit_u8(&cg, 0xC4); cg_emit_u8(&cg, (u8)alloc_total);
    } else {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x81);
        cg_emit_u8(&cg, 0xC4); cg_emit_u32(&cg, alloc_total);
    }
    static const u8 epi[] = {
        0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D,
        0x41, 0x5C, 0x5B, 0x5D, 0xC3,
    };
    cg_emit_bytes(&cg, epi, sizeof(epi));

    /* Common failure epilogue: eax=1, same teardown. */
    u32 fail_off = cg_off(&cg);
    cg_emit_u8(&cg, 0xB8);              /* mov eax, 1 */
    cg_emit_u32(&cg, 1);
    if (alloc_total < 0x80) {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x83);
        cg_emit_u8(&cg, 0xC4); cg_emit_u8(&cg, (u8)alloc_total);
    } else {
        cg_emit_u8(&cg, 0x48); cg_emit_u8(&cg, 0x81);
        cg_emit_u8(&cg, 0xC4); cg_emit_u32(&cg, alloc_total);
    }
    cg_emit_bytes(&cg, epi, sizeof(epi));

    /* Patch each guard entry: success returns (snap=0xFFFE) point
     * at success_off; everything else points at fail_off. */
    for (u32 g = 0; g < cg.guard_count; g++) {
        u32 target = (cg.guards[g].snap_idx == 0xFFFE)
                     ? success_off : fail_off;
        cg_patch_rel32(&cg, cg.guards[g].je_disp_off, target);
    }

    if (cg.failed) {
        cando_mcode_free(&t->mcode);
        return false;
    }

    cando_mcode_finalize(&t->mcode);
    t->mcode.size = cg_off(&cg);
    t->func_mcode_fn = (CandoFuncTraceFn)t->mcode.base;
    return true;
}

bool cando_jit_codegen_trace(struct CandoVM *vm, CandoTrace *t) {
    if (!t) return false;
    if (t->is_function_trace) {
        if (t->func_mcode_fn != NULL) return true;
        return cando_jit_codegen_func_trace(vm, t);
    }
    if (t->mcode_fn != NULL) return true;
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
    /* Pin loop-carried numeric scalars in xmm6+.  Reads IRF_NUM_KNOWN
     * set by mark_known_num_sloads.  Body emit and side-exit stubs
     * consult cg.pinned_slots / pinned_first_load. */
    cg_assign_pinned_slots(&cg, &t->ir);

    emit_prologue(&cg);
    emit_pinned_prologue(&cg);

    /* Phase 8.9: capture body-start offset so IR_LOOP can emit a
     * backwards jump (instead of falling off the end and returning
     * to cando_trace_run for the next iter).  Saves ~30-50ns/iter
     * of function-call dispatch overhead on hot loops. */
    u32 body_start_off = cg_off(&cg);

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
            emit_sload(&cg, in->op1, i, (IRType)in->type,
                       (in->flags & IRF_NUM_KNOWN) != 0);
            break;
        case IR_SSTORE:
            /* Phase 4.4g: raw u64 copy works for both IRT_NUM and
             * IRT_OBJ source -- emit_sstore is just mov rax mem.
             * NaN canonicalization (cando_number) was a pre-existing
             * gap on the codegen path; not regressed here.
             *
             * Phase 4.4 v1c: IRF_SUNK on the SSTORE means escape
             * analysis decided this store is dead within the trace.
             * Skip the emit AND record a sink_rec so the side-exit
             * stub materialises a real heap object before bytecode
             * resumes. */
            if (in->flags & IRF_SUNK) {
                CGSunk *s = cg_find_sunk(&cg, in->op2);
                if (s) {
                    if (cg.sink_count < CG_MAX_SUNK) {
                        CGSinkRec *r = &cg.sink_recs[cg.sink_count++];
                        r->slot       = in->op1;
                        r->stack_off  = s->stack_off;
                        r->capacity   = (s->is_array ? s->cursor : s->capacity);
                        r->is_array   = s->is_array;
                        if (!s->is_array) {
                            for (u32 f = 0; f < s->capacity; f++)
                                r->field_kref[f] = s->field_kref[f];
                        }
                    }
                    break;
                }
                /* Phase 4.4 v1d: defensive fall-through.  The IRF_SUNK
                 * flag was set on an alloc the codegen doesn't track
                 * (e.g. RANGE_ASC slipped through escape_analysis in
                 * an older build).  Don't silently drop the store --
                 * emit it normally so the slot at least gets written. */
            }
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
            /* Phase 8.2: IRT_OBJ globals resolve to CdoObject* and
             * cache the pointer in vals[i].  Downstream IR_INDEX_GET
             * / IR_INDEX_SET detect ptr-source via
             * ir_ref_holds_arr_ptr and use the inline fast path
             * (no per-access lock + no handle resolve). */
            if (in->type == IRT_OBJ) {
                emit_gload_arr(&cg, &t->ir, in->op1, i);
            } else if (in->type == IRT_NUM) {
                emit_gload(&cg, &t->ir, in->op1, i, is_inv);
            } else {
                cg.failed = true;
            }
            break;
        case IR_GSTORE:
            emit_gstore(&cg, &t->ir, in->op1, in->op2, is_inv);
            break;
        case IR_HLOAD_SLOT:
            emit_hload_slot(&cg, in->op1, i);
            break;
        case IR_AREF:
            /* Phase 8.2: IR_AREF's op1 is always a pointer (from
             * IR_HLOAD_SLOT) so the inline path is always usable. */
            emit_index_get_inline(&cg, in->op1, in->op2, i);
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
            /* Phase 8.2: when op1 is from IR_GLOAD-IRT_OBJ or
             * IR_HLOAD_SLOT, vals[op1] holds CdoObject* directly --
             * use the inline fast path. */
            if (ir_ref_holds_arr_ptr(&t->ir, in->op1)) {
                emit_index_get_inline(&cg, in->op1, in->op2, i);
            } else {
                emit_index_get(&cg, in->op1, in->op2, i);
            }
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
            /* Phase 8.2: ptr-source fast path. */
            if (ir_ref_holds_arr_ptr(&t->ir, in->op1)) {
                emit_index_set_inline(&cg, in->op1, in->op2, val_ref);
            } else {
                emit_index_set(&cg, in->op1, in->op2, val_ref);
            }
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
        case IR_HLEN:
            emit_hlen(&cg, &t->ir, in->op1, i);
            break;
        case IR_LOOP: {
            /* Phase 8.9: do the loop INTERNALLY in mcode rather than
             * returning LOOP_DONE per-iter to cando_trace_run.  Saves
             * ~30-50 ns/iter of dispatch overhead.
             *
             * Order matters: the LOOP_DONE epilogue's stack-to-shadow
             * sync (Phase 4.4 v1d) is no longer reached via fall-
             * through, so we replicate that here before jumping back.
             * Also bump run_iter_count for jit-stats. */
            if (cg.sunk_total_bytes > 0) {
                emit_mov_rax_r12_off(&cg, (i32)offsetof(struct CandoTrace,
                                                         sink_shadow));
                u32 slots = cg.sunk_total_bytes / 8;
                for (u32 si = 0; si < slots; si++) {
                    emit_mov_rcx_rbp_off(&cg, -(i32)(56 + 8 * si));
                    emit_mov_rax_off_rcx(&cg, (i32)(8 * si));
                }
                emit_mov_r12_off_byte_imm(&cg,
                    (i32)offsetof(struct CandoTrace, sink_shadow_init), 1);
            }
            emit_inc_dword_r12_off(&cg,
                (i32)offsetof(struct CandoTrace, run_iter_count));
            emit_mov_r13b_one(&cg);
            u32 here_after_jmp = cg_off(&cg) + 5;  /* end of jmp rel32 */
            i32 disp = (i32)body_start_off - (i32)here_after_jmp;
            emit_jmp_rel32(&cg, disp);
            /* xmm0 cache state across the back-jump is unknown. */
            cg_invalidate_xmm0(&cg);
            break;
        }
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

    /* Phase 4.4 v1d: at LOOP_DONE, sync stack buffer back to the
     * heap shadow + flip sink_shadow_init=1.  The shadow now
     * reflects this iter's writes; next iter's prologue will
     * pre-fill the stack buffer with these values, so a side-
     * exit BEFORE any FIELD_SET / APPEND of the next iter still
     * materialises a valid object (= last completed iter). */
    if (cg.sunk_total_bytes > 0) {
        emit_mov_rax_r12_off(&cg, (i32)offsetof(struct CandoTrace,
                                                 sink_shadow));
        u32 slots = cg.sunk_total_bytes / 8;
        for (u32 i = 0; i < slots; i++) {
            emit_mov_rcx_rbp_off(&cg, -(i32)(56 + 8 * i));
            emit_mov_rax_off_rcx(&cg, (i32)(8 * i));
        }
        emit_mov_r12_off_byte_imm(&cg,
            (i32)offsetof(struct CandoTrace, sink_shadow_init), 1);
    }

    /* LOOP_DONE epilogue: returns 0 (TRACE_LOOP_DONE).  Pinned regs
     * mirror to vals[first_load] is already kept up-to-date by SLOAD,
     * so a clean LOOP_DONE doesn't need to re-flush -- but we still
     * write each pinned slot back to its frame slot here, since this
     * exit path bypasses snapshot replay. */
    if (cg.pinned_slot_count > 0) {
        for (u32 k = 0; k < cg.pinned_slot_count; k++)
            emit_movsd_slot_xmmN(&cg, cg.pinned_slots[k], (u8)(6 + k));
    }
    emit_xor_eax_eax(&cg);
    emit_epilogue(&cg);

    /* Stub area: side-exit replay calls run AFTER the snapshot has
     * already been built; we don't need to preserve pinned xmm regs
     * across those calls.  Switch the spill-wrap off. */
    cg.in_stub_emit = true;

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
     *   ; Phase 4.4 v1c: stash snap_idx in r9 (callee-saved across
     *   ; the materialise call -- it's preserved on entry by us
     *   ; because we don't touch r9 in the helper, but to be safe
     *   ; the materialise helper's ABI doesn't take r9 anyway).
     *
     *   ; Phase 4.4 v1c materialise call (only emitted when there
     *   ; are actually sink_recs to process):
     *   mov rdi, rbx          -- vm
     *   mov rsi, r12          -- t
     *   mov rdx, rbp          -- rbp_base
     *   mov rcx, r15          -- frame_slots
     *   movabs rax, materialize_helper
     *   call rax
     *
     *   ; Snapshot replay (always):
     *   mov rdi, rbx          -- vm
     *   mov rsi, r12          -- t
     *   mov rdx, r14          -- vals
     *   mov rcx, r15          -- frame_slots
     *   mov r8,  r9           -- snap_idx
     *   movabs rax, replay_helper
     *   call rax
     *   mov eax, 1            -- TRACE_GUARD_FAILED
     *   epilogue
     */
    u32 common_off = cg_off(&cg);
    /* Pinned-slot flush.  For each pinned slot, write the current
     * xmm reg back to frame_slots[slot] BEFORE snapshot replay.
     * This guarantees a correct slot value even when the failing
     * guard's snapshot doesn't include an entry for this slot (i.e.
     * the failing guard fires before any same-iter SSTORE).  If the
     * guard fires after an SSTORE, the snapshot's SNAP_SLOT entry
     * will overwrite with vals[first_load] = iter-start value -- so
     * the SLOAD's mirror at iter start is what ultimately wins. */
    for (u32 k = 0; k < cg.pinned_slot_count; k++)
        emit_movsd_slot_xmmN(&cg, cg.pinned_slots[k], (u8)(6 + k));
    if (cg.sink_count > 0) {
        /* r9 carries snap_idx for the replay helper; the
         * materialise call would clobber it.  Stash in r13
         * (callee-saved). */
        emit_mov_r13_r9(&cg);
        emit_mov_rdi_rbx(&cg);
        emit_mov_rsi_r12(&cg);
        emit_mov_rdx_rbp(&cg);
        emit_mov_rcx_r15(&cg);
        emit_movabs_rax(&cg,
            (u64)(uintptr_t)&cando_jit_materialize_sunk_for_mcode);
        emit_call_rax(&cg);
        emit_mov_r9_r13(&cg);
    }
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

    /* Phase 4.4 v1c: transfer sink_recs to the trace so the
     * side-exit materialise helper can iterate them.  Allocated
     * lazily; trace_release_storage frees it. */
    if (cg.sink_count > 0) {
        size_t bytes = sizeof(CandoSinkRec) * cg.sink_count;
        t->sink_recs = (CandoSinkRec *)cando_alloc(bytes);
        if (!t->sink_recs) {
            cando_mcode_free(&t->mcode);
            return false;
        }
        for (u32 i = 0; i < cg.sink_count; i++) {
            CandoSinkRec *dst = &t->sink_recs[i];
            const CGSinkRec *src = &cg.sink_recs[i];
            dst->slot      = src->slot;
            dst->stack_off = src->stack_off;
            dst->capacity  = src->capacity;
            dst->is_array  = src->is_array;
            for (u32 f = 0; f < CANDO_SINK_MAX_FIELDS &&
                            f < CG_MAX_OBJ_FIELDS; f++)
                dst->field_kref[f] = src->field_kref[f];
        }
        t->sink_rec_count = cg.sink_count;
        t->sink_rec_cap   = cg.sink_count;
    }

    /* Phase 4.4 v1d: allocate the heap-persistent shadow buffer
     * matching the mcode's stack reservation.  Zeroed; flips to
     * "valid" via sink_shadow_init=1 at first LOOP_DONE.  Refer
     * to the prologue / LOOP_DONE / materialise comments above. */
    if (cg.sunk_total_bytes > 0) {
        t->sink_shadow = cando_alloc(cg.sunk_total_bytes);
        if (!t->sink_shadow) {
            cando_mcode_free(&t->mcode);
            cando_free(t->sink_recs);
            t->sink_recs = NULL;
            t->sink_rec_count = 0;
            t->sink_rec_cap = 0;
            return false;
        }
        memset(t->sink_shadow, 0, cg.sunk_total_bytes);
        t->sink_shadow_bytes = cg.sunk_total_bytes;
        t->sink_shadow_init  = 0;
    }

    t->mcode.written = (u32)(cg.cur - cg.base);
    if (!cando_mcode_finalize(&t->mcode)) {
        cando_mcode_free(&t->mcode);
        cando_free(t->sink_recs);
        t->sink_recs = NULL;
        t->sink_rec_count = 0;
        t->sink_rec_cap = 0;
        cando_free(t->sink_shadow);
        t->sink_shadow = NULL;
        t->sink_shadow_bytes = 0;
        t->sink_shadow_init = 0;
        return false;
    }
    t->mcode_fn = (CandoTraceStatus (*)(struct CandoVM *, CandoTrace *,
                                        bool, CandoValue *, TraceVal *))
                  (uintptr_t)cg.base;
    return true;
}
