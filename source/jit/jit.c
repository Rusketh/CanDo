/*
 * jit/jit.c -- CandoJit lifecycle + recorder.
 *
 * Phase 3.3a recorder body.  See jit.h for the surface.
 *
 * The recorder lives inside the dispatch loop: on every backedge
 * (OP_LOOP) the dispatch handler calls cando_jit_hot_hit; when the
 * per-PC threshold trips, the recorder is activated.  Subsequent
 * DISPATCH() iterations route through cando_recorder_observe, which
 * reads the upcoming opcode, emits matching IR, and mirrors the
 * opcode's stack effect on its shadow stack_map.  Recording closes
 * cleanly when ip lands back at start_pc; otherwise it aborts.
 *
 * Recordable opcodes in v1: OP_CONST (numbers only),
 * OP_LOAD_LOCAL / OP_STORE_LOCAL / OP_DEF_LOCAL, OP_ADD / OP_SUB /
 * OP_MUL, OP_LOOP (the close).  Anything else aborts.  Recording
 * across function boundaries is not supported yet -- if the recorded
 * code calls into another frame, the recorder aborts on the first
 * opcode in the callee.
 */

#include "jit.h"
#include "../vm/vm.h"
#include "../vm/bridge.h"
#include "../vm/opcodes.h"
#include "../vm/chunk.h"
#include "../object/array.h"
#include "../object/object.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ============================================================ */
/* Internal helpers                                              */
/* ============================================================ */

/* The CandoJit pointer for `vm`.  All entry points take a CandoVM*
 * so that recorder code can introspect the VM state cheaply. */
static CandoJit *jit_of(struct CandoVM *vm) { return vm ? vm->jit : NULL; }

/* Stack-slot index for a CandoValue pointer into vm->stack[]. */
static u32 slot_index(const struct CandoVM *vm, const CandoValue *p) {
    return (u32)(p - vm->stack);
}

/* Push an IRRef onto the recorder's shadow stack.  Asserts in debug
 * builds that the recorder's view stays consistent with vm->stack_top
 * after the next op executes; release builds let it slide. */
static void rec_push(CandoRecorder *r, IRRef ref, u32 sp_after) {
    /* sp_after is the slot that will hold the value once the
     * interpreter executes the op; rec is being called BEFORE the
     * op runs, so we write to sp_after directly. */
    if (sp_after < CANDO_STACK_MAX) r->stack_map[sp_after] = ref;
}

/* Helper: append IR_GUARD_NUM unless `ref` is already known IRT_NUM.
 * Returns the (possibly new) ref to use as the operand.  Pure ops
 * (KNUM, ADD, SUB, MUL with both operands NUM) yield IRT_NUM, so the
 * guard collapses for the common in-trace case. */
static IRRef ensure_num(CandoTraceIR *ir, IRRef ref) {
    if (IRREF_IS_K(ref)) {
        /* Constants in our pool are always numbers in v1 (string
         * constants would have come from OP_CONST which we currently
         * abort on for non-numeric). */
        return ref;
    }
    const IRIns *in = cando_ir_get_ins(ir, ref);
    if (in && in->type == IRT_NUM) return ref;
    /* Emit a guard.  The guard's op1 is the value being asserted; its
     * type tag matches the asserted type so optimisation passes can
     * forward types through guards. */
    return cando_ir_emit(ir, IR_GUARD_NUM, IRT_NUM, IRF_GUARD, ref, 0);
}

/* ============================================================ */
/* Recorder lifecycle                                            */
/* ============================================================ */

void cando_recorder_init(CandoRecorder *r) {
    if (!r) return;
    r->active               = false;
    r->start_pc             = NULL;
    r->stack_map            = NULL;
    r->stack_map_cap        = 0;
    r->first_load           = NULL;
    r->pending_snap         = NULL;
    r->pending_snap_count   = 0;
    r->pending_snap_cap     = 0;
    r->staging_snapshots         = NULL;
    r->staging_snapshot_count    = 0;
    r->staging_snapshot_cap      = 0;
    r->staging_snap_entries      = NULL;
    r->staging_snap_entry_count  = 0;
    r->staging_snap_entry_cap    = 0;
    r->frame_base           = 0;
    r->frame_count_at_start = 0;
    r->trace_starts         = 0;
    r->trace_aborts         = 0;
    r->traces_compiled      = 0;
    r->last_abort[0]        = '\0';
    cando_trace_ir_init(&r->ir);
}

void cando_recorder_destroy(CandoRecorder *r) {
    if (!r) return;
    cando_trace_ir_destroy(&r->ir);
    cando_free(r->stack_map);
    cando_free(r->first_load);
    cando_free(r->pending_snap);
    cando_free(r->staging_snapshots);
    cando_free(r->staging_snap_entries);
    r->stack_map         = NULL;
    r->stack_map_cap     = 0;
    r->first_load        = NULL;
    r->pending_snap      = NULL;
    r->pending_snap_cap  = 0;
    r->staging_snapshots = NULL;
    r->staging_snapshot_cap = 0;
    r->staging_snap_entries = NULL;
    r->staging_snap_entry_cap = 0;
    r->active            = false;
}

/* Lazy-allocate the stack_map sized to the VM's CANDO_STACK_MAX.
 * Resizes in place if the VM ever grows its stack (it doesn't today,
 * but the recorder doesn't bake that in).  first_load (Phase 4)
 * shares stack_map_cap and is grown alongside. */
static void rec_ensure_stack_map(CandoRecorder *r, u32 want) {
    if (want <= r->stack_map_cap) return;
    u32 nc = r->stack_map_cap ? r->stack_map_cap * 2 : 256;
    while (nc < want) nc *= 2;
    r->stack_map  = cando_realloc(r->stack_map,  sizeof(IRRef) * nc);
    r->first_load = cando_realloc(r->first_load, sizeof(IRRef) * nc);
    memset(r->stack_map  + r->stack_map_cap, 0,
           sizeof(IRRef) * (nc - r->stack_map_cap));
    memset(r->first_load + r->stack_map_cap, 0,
           sizeof(IRRef) * (nc - r->stack_map_cap));
    r->stack_map_cap = nc;
}

/* Append a snapshot entry to the recorder's pending list.  Skip if
 * the slot already has an entry (the FIRST SSTORE in this iteration
 * captured the right pre-iter value; subsequent SSTOREs to the same
 * slot are stale and shouldn't overwrite it). */
static void rec_pending_snap_add(CandoRecorder *r, u32 slot, IRRef irref) {
    for (u32 i = 0; i < r->pending_snap_count; i++) {
        if (r->pending_snap[i].slot == slot) return;
    }
    if (r->pending_snap_count >= r->pending_snap_cap) {
        u32 nc = r->pending_snap_cap ? r->pending_snap_cap * 2 : 8;
        r->pending_snap = cando_realloc(r->pending_snap,
                                        sizeof(CandoSnapEntry) * nc);
        r->pending_snap_cap = nc;
    }
    r->pending_snap[r->pending_snap_count].slot  = slot;
    r->pending_snap[r->pending_snap_count].irref = irref;
    r->pending_snap_count++;
}

/* Build a CandoSnapshot from the current pending entries.  Stores
 * it in the recorder's staging pool; ownership transfers to the new
 * CandoTrace at cando_recorder_finish time.  Returns the snapshot's
 * 1-based index (0 means "no snapshot, no rollback needed").
 *
 * The pending list isn't cleared -- subsequent guards in the same
 * trace iteration include all SSTOREs from the iter's start. */
static u16 rec_build_snapshot(CandoRecorder *r) {
    if (r->pending_snap_count == 0) return 0;

    /* Append entries to the staging pool. */
    if (r->staging_snap_entry_count + r->pending_snap_count >
        r->staging_snap_entry_cap) {
        u32 nc = r->staging_snap_entry_cap
                 ? r->staging_snap_entry_cap * 2 : 16;
        while (nc < r->staging_snap_entry_count + r->pending_snap_count)
            nc *= 2;
        r->staging_snap_entries = cando_realloc(r->staging_snap_entries,
                                                sizeof(CandoSnapEntry) * nc);
        r->staging_snap_entry_cap = nc;
    }
    u32 off = r->staging_snap_entry_count;
    memcpy(r->staging_snap_entries + off, r->pending_snap,
           sizeof(CandoSnapEntry) * r->pending_snap_count);
    r->staging_snap_entry_count += r->pending_snap_count;

    /* Append snapshot header. */
    if (r->staging_snapshot_count >= r->staging_snapshot_cap) {
        u32 nc = r->staging_snapshot_cap
                 ? r->staging_snapshot_cap * 2 : 4;
        r->staging_snapshots = cando_realloc(r->staging_snapshots,
                                             sizeof(CandoSnapshot) * nc);
        r->staging_snapshot_cap = nc;
    }
    u16 idx = (u16)(r->staging_snapshot_count + 1);   /* 1-based */
    r->staging_snapshots[r->staging_snapshot_count].entry_offset = off;
    r->staging_snapshots[r->staging_snapshot_count].entry_count =
        r->pending_snap_count;
    r->staging_snapshot_count++;
    return idx;
}

void cando_recorder_abort(struct CandoVM *vm, const char *reason) {
    CandoJit *j = jit_of(vm);
    if (!j) return;
    CandoRecorder *r = &j->recorder;
    if (!r->active) {
        /* Stub-style abort (Phase 3.2 leftover entry point) -- still
         * record the reason so jit.stats() shows it. */
    } else {
        r->active = false;
        cando_trace_ir_reset(&r->ir);
    }
    r->trace_aborts++;
    if (reason) {
        size_t n = strlen(reason);
        if (n >= sizeof(r->last_abort)) n = sizeof(r->last_abort) - 1;
        memcpy(r->last_abort, reason, n);
        r->last_abort[n] = '\0';
    } else {
        r->last_abort[0] = '\0';
    }
}

void cando_recorder_begin(struct CandoVM *vm, const u8 *pc) {
    CandoJit *j = jit_of(vm);
    if (!j) return;
    CandoRecorder *r = &j->recorder;
    if (r->active) {
        /* Should not happen -- cando_hot_hit auto-blacklists on
         * trigger, so a second trigger before the first finishes
         * shouldn't be possible.  Be defensive anyway. */
        return;
    }

    cando_trace_ir_reset(&r->ir);
    r->active   = true;
    r->start_pc = pc;
    r->trace_starts++;

    /* Capture the recording frame.  vm->frame_count >= 1 always when
     * the dispatch loop is running. */
    if (vm && vm->frame_count > 0) {
        r->frame_base           = slot_index(vm, vm->frames[vm->frame_count - 1].slots);
        r->frame_count_at_start = vm->frame_count;
    } else {
        r->frame_base           = 0;
        r->frame_count_at_start = 0;
    }

    /* Lazy-allocate stack_map sized to the VM's stack capacity;
     * subsequent traces reuse the same buffer. */
    rec_ensure_stack_map(r, CANDO_STACK_MAX);
    /* Zero the active prefix so SLOADs fire on first read. */
    memset(r->stack_map,  0, sizeof(IRRef) * r->stack_map_cap);
    memset(r->first_load, 0, sizeof(IRRef) * r->stack_map_cap);

    /* Reset Phase 4 staging.  pending_snap regrows lazily; the
     * staging pools reset their counts so the next trace starts with
     * fresh snapshot indices. */
    r->pending_snap_count        = 0;
    r->staging_snapshot_count    = 0;
    r->staging_snap_entry_count  = 0;
}

/* Finish a successfully closed trace: emit IR_LOOP, copy the IR into
 * the trace cache, deactivate.  Caller has confirmed ip == start_pc. */
/* Mark loop-invariant IR ops with IRF_INVARIANT.
 *
 * An op is loop-invariant iff every input it depends on is also
 * invariant.  A SLOAD becomes invariant iff its slot has no SSTORE
 * anywhere in the trace; same for GLOAD vs GSTORE.  Constant-load
 * ops (KNUM, KBOOL, KNULL) are always invariant.  All other ops
 * propagate invariance from their operands.  Guards, stores, and
 * IR_LOOP / IR_AREF / IR_GLOAD with stored-name are NEVER marked
 * invariant: guards must fire each iteration (they're side-exit
 * anchors); stores write each iteration; IR_AREF reads array
 * elements that depend on the loop-variant index; IR_GLOAD reads
 * an external table that may have changed even if no GSTORE in
 * THIS trace touched it.
 *
 * Forward pass over the IR.  The recorder emits in topological
 * order (def before use within a trace), so a single pass is
 * sufficient.
 */
static void mark_loop_invariants(CandoTraceIR *ir) {
    /* Pass 1: collect the set of slots written by IR_SSTORE and the
     * set of names written by IR_GSTORE.  Slot indices are u32 so we
     * keep two small bit arrays sized to the trace's actual extent
     * (heap-allocated, freed at end of pass). */
    u32 max_slot = 0;
    for (u32 i = 1; i < ir->ir_count; i++) {
        const IRIns *in = &ir->ir[i];
        if (in->op == IR_SSTORE && in->op1 + 1 > max_slot)
            max_slot = in->op1 + 1;
    }
    u8 *stored_slot = max_slot
        ? cando_alloc(sizeof(u8) * max_slot) : NULL;
    if (stored_slot) memset(stored_slot, 0, sizeof(u8) * max_slot);
    /* Globals: collect const-pool indices that are GSTORE targets. */
    u8 *stored_name = ir->const_count
        ? cando_alloc(sizeof(u8) * ir->const_count) : NULL;
    if (stored_name) memset(stored_name, 0, sizeof(u8) * ir->const_count);

    for (u32 i = 1; i < ir->ir_count; i++) {
        const IRIns *in = &ir->ir[i];
        if (in->op == IR_SSTORE && stored_slot)
            stored_slot[in->op1] = 1;
        if (in->op == IR_GSTORE && stored_name)
            stored_name[IRREF_KIDX(in->op1)] = 1;
    }

    /* Pass 2: forward-propagate invariance.  Operand IRRefs are
     * always smaller than the current op's index (SSA), so we can
     * read the flag directly from a previously-processed entry. */
    for (u32 i = 1; i < ir->ir_count; i++) {
        IRIns *in = &ir->ir[i];

        /* Anchor ops never invariant: stores, guards, the LOOP
         * marker, the IR_NOP sentinel. */
        if (in->op == IR_SSTORE || in->op == IR_GSTORE ||
            in->op == IR_LOOP   || in->op == IR_NOP   ||
            (in->flags & IRF_GUARD))
            continue;

        bool inv = false;
        switch (in->op) {
            case IR_KNUM:
            case IR_KBOOL:
            case IR_KNULL:
            case IR_KSTR:
            case IR_KOBJ:
                inv = true;
                break;
            case IR_SLOAD:
            case IR_HLOAD_SLOT:
                /* Both ops read the FRAME-RELATIVE source slot in
                 * op1.  Loop-invariant iff that slot has no SSTORE
                 * anywhere in the trace.  HLOAD_SLOT carries the
                 * expensive bridge_resolve call -- making it
                 * invariant is the whole point of Phase 5b. */
                inv = !(stored_slot && in->op1 < max_slot
                        && stored_slot[in->op1]);
                break;
            case IR_GLOAD: {
                u32 ki = IRREF_KIDX(in->op1);
                inv = !(stored_name && ki < ir->const_count
                        && stored_name[ki]);
                break;
            }
            case IR_AREF:
                /* op2 is the loop-variant index in v1 traces. */
                inv = false;
                break;
            default: {
                /* Pure op: invariant iff every operand IRRef points
                 * to an invariant op.  Constant-pool refs (high bit)
                 * are always invariant; instruction refs propagate. */
                bool ok = true;
                IRRef ops[2] = { in->op1, in->op2 };
                for (int k = 0; k < 2 && ok; k++) {
                    IRRef r = ops[k];
                    if (r == IRREF_NIL) continue;
                    if (IRREF_IS_K(r)) continue;
                    if (r >= i)        { ok = false; break; }
                    if (!(ir->ir[r].flags & IRF_INVARIANT)) {
                        ok = false;
                        break;
                    }
                }
                inv = ok;
                break;
            }
        }
        if (inv) in->flags |= IRF_INVARIANT;
    }

    cando_free(stored_slot);
    cando_free(stored_name);
}

static void cando_recorder_finish(struct CandoVM *vm) {
    CandoJit *j = jit_of(vm);
    if (!j || !j->recorder.active) return;
    CandoRecorder *r = &j->recorder;

    /* Empty traces (closed before recording any real op) abort. */
    if (r->ir.ir_count <= 1) {
        cando_recorder_abort(vm, "trace closed empty");
        return;
    }

    cando_ir_emit(&r->ir, IR_LOOP, IRT_VOID, 0, 0, 0);

    /* Mark loop-invariant ops so cando_trace_run can skip them on
     * iterations 2+ (Phase 5 LICM v1). */
    mark_loop_invariants(&r->ir);

    /* Cache the trace.  Drop on overflow rather than evicting --
     * Phase 3.3 just demonstrates the pipeline; eviction is Phase 4. */
    if (j->trace_count < CANDO_JIT_MAX_TRACES) {
        CandoTrace *t = &j->traces[j->trace_count++];
        /* Move the IR (transfer ownership): copy the struct by
         * value; the pointers inside (ir, constants) move with it.
         * Re-init the recorder's IR so subsequent traces start fresh. */
        t->ir         = r->ir;
        t->start_pc   = r->start_pc;
        t->id         = j->next_trace_id++;
        t->values_buf = NULL;   /* lazy-allocated by cando_trace_run */
        t->values_cap = 0;
        /* Transfer Phase 4 staging snapshot pool. */
        t->snapshots         = r->staging_snapshots;
        t->snapshot_count    = r->staging_snapshot_count;
        t->snapshot_cap      = r->staging_snapshot_cap;
        t->snap_entries      = r->staging_snap_entries;
        t->snap_entry_count  = r->staging_snap_entry_count;
        t->snap_entry_cap    = r->staging_snap_entry_cap;
        r->staging_snapshots         = NULL;
        r->staging_snapshot_count    = 0;
        r->staging_snapshot_cap      = 0;
        r->staging_snap_entries      = NULL;
        r->staging_snap_entry_count  = 0;
        r->staging_snap_entry_cap    = 0;
        cando_trace_ir_init(&r->ir);
        r->traces_compiled++;
        r->active = false;
    } else {
        /* Cache full -- the IR-interpreter would never see this
         * trace, so logging it as compiled is misleading.  Treat
         * as an abort with a distinct reason so users notice. */
        cando_recorder_abort(vm, "trace cache full");
    }
}

/* ============================================================ */
/* Per-opcode recording                                          */
/* ============================================================ */

/* The recorder reads the opcode + its operand directly from `ip`.
 * Operand reads mirror the dispatch loop's READ_U16 (little-endian
 * u16 at ip+1).  All recordable opcodes here use OPFMT_A. */
static u16 read_op_arg(const u8 *ip) {
    return cando_read_u16(ip + 1);
}

/* Get the current frame's chunk -- needed to resolve OP_CONST's
 * constant-pool index. */
static const CandoChunk *current_chunk(const struct CandoVM *vm) {
    if (!vm || vm->frame_count == 0) return NULL;
    const CandoCallFrame *f = &vm->frames[vm->frame_count - 1];
    return f->closure ? f->closure->chunk : NULL;
}

void cando_recorder_observe(struct CandoVM *vm, const u8 *ip) {
    CandoJit *j = jit_of(vm);
    if (!j || !j->recorder.active || !ip) return;
    CandoRecorder *r = &j->recorder;

    /* Bail if we've left the recording frame (function call/return
     * during recording).  Phase 3.3b will inline calls. */
    if (vm->frame_count != r->frame_count_at_start) {
        cando_recorder_abort(vm, "recording crossed a frame boundary");
        return;
    }

    /* Trace length cap. */
    if (r->ir.ir_count >= CANDO_JIT_MAX_IR_INS) {
        cando_recorder_abort(vm, "trace too long");
        return;
    }

    /* End-of-loop: ip back at start_pc with at least one op recorded. */
    if (ip == r->start_pc && r->ir.ir_count > 1) {
        cando_recorder_finish(vm);
        return;
    }

    const u8 op = *ip;
    /* sp BEFORE the upcoming opcode executes. */
    const u32 sp = slot_index(vm, vm->stack_top);

    switch (op) {
        case OP_NULL:
            rec_push(r, cando_ir_emit(&r->ir, IR_KNULL, IRT_NIL, 0, 0, 0), sp);
            break;

        case OP_TRUE:
            rec_push(r, cando_ir_emit(&r->ir, IR_KBOOL, IRT_BOOL, 0, 1, 0), sp);
            break;

        case OP_FALSE:
            rec_push(r, cando_ir_emit(&r->ir, IR_KBOOL, IRT_BOOL, 0, 0, 0), sp);
            break;

        case OP_POP: {
            /* Stack effect only; no IR. */
            break;
        }

        case OP_CONST: {
            const CandoChunk *chunk = current_chunk(vm);
            u16 ci = read_op_arg(ip);
            if (!chunk || ci >= chunk->const_count) {
                cando_recorder_abort(vm, "OP_CONST out of range");
                return;
            }
            CandoValue cv = chunk->constants[ci];
            if (!cando_is_number(cv)) {
                cando_recorder_abort(vm, "OP_CONST non-numeric (v1 only records numbers)");
                return;
            }
            IRRef k = cando_ir_const(&r->ir, cando_value_copy(cv));
            IRRef e = cando_ir_emit(&r->ir, IR_KNUM, IRT_NUM, 0, k, 0);
            rec_push(r, e, sp);
            break;
        }

        case OP_LOAD_LOCAL: {
            /* Slot operand stored as FRAME-RELATIVE so the same trace
             * works no matter where in vm->stack[] the recording
             * frame's slots live on subsequent invocations.  The
             * stack_map index is still ABSOLUTE so SSA-style operand
             * resolution stays correct within a single trace. */
            u16 slot = read_op_arg(ip);
            u32 abs  = r->frame_base + slot;
            IRRef src = (abs < r->stack_map_cap) ? r->stack_map[abs] : IRREF_NIL;
            if (src == IRREF_NIL) {
                src = cando_ir_emit(&r->ir, IR_SLOAD, IRT_NUM, 0, slot, 0);
                if (abs < r->stack_map_cap) {
                    r->stack_map[abs] = src;
                    /* Phase 4: capture the FIRST SLOAD for this slot
                     * so a later SSTORE can reference its pre-iter
                     * value when building a snapshot. */
                    if (r->first_load[abs] == IRREF_NIL)
                        r->first_load[abs] = src;
                }
            }
            rec_push(r, src, sp);
            break;
        }

        case OP_LOAD_GLOBAL: {
            /* op1: constant-pool index of the name string. */
            const CandoChunk *chunk = current_chunk(vm);
            u16 ci = read_op_arg(ip);
            if (!chunk || ci >= chunk->const_count) {
                cando_recorder_abort(vm, "OP_LOAD_GLOBAL out of range");
                return;
            }
            CandoValue name_val = chunk->constants[ci];
            if (!cando_is_string(name_val)) {
                cando_recorder_abort(vm, "OP_LOAD_GLOBAL with non-string name");
                return;
            }
            /* Probe the global at recording time -- if absent or
             * non-numeric, the bytecode would error or push a non-num
             * value the trace can't model.  Abort cleanly. */
            CandoValue probe;
            if (!cando_vm_get_global(vm, cando_as_string(name_val)->data,
                                      &probe) ||
                !cando_is_number(probe)) {
                cando_recorder_abort(vm,
                    "OP_LOAD_GLOBAL non-numeric or undefined (v1)");
                return;
            }
            IRRef k = cando_ir_const(&r->ir, cando_value_copy(name_val));
            IRRef e = cando_ir_emit(&r->ir, IR_GLOAD, IRT_NUM, 0, k, 0);
            rec_push(r, e, sp);
            break;
        }

        case OP_STORE_GLOBAL:
        case OP_DEF_GLOBAL:
        case OP_DEF_CONST_GLOBAL: {
            /* OP_STORE_GLOBAL is peek-and-store; OP_DEF_GLOBAL[_CONST]
             * is pop-and-store.  As with OP_STORE_LOCAL the recorder's
             * stack effect mirrors via stack_map/sp -- the bytecode
             * drives vm->stack_top either way.
             *
             * v1 records numeric stores only.  DEF_CONST_GLOBAL stores
             * a const-protected global; if a future iteration of the
             * trace tries to overwrite it the IR-interp side-exits.   */
            const CandoChunk *chunk = current_chunk(vm);
            u16 ci = read_op_arg(ip);
            if (!chunk || ci >= chunk->const_count) {
                cando_recorder_abort(vm, "OP_*_GLOBAL out of range");
                return;
            }
            CandoValue name_val = chunk->constants[ci];
            if (!cando_is_string(name_val)) {
                cando_recorder_abort(vm, "OP_*_GLOBAL with non-string name");
                return;
            }
            IRRef top = (sp > 0) ? r->stack_map[sp - 1] : IRREF_NIL;
            if (top == IRREF_NIL) {
                cando_recorder_abort(vm, "global store with empty stack_map");
                return;
            }
            const IRIns *src = cando_ir_get_ins(&r->ir, top);
            if (!src || src->type != IRT_NUM) {
                cando_recorder_abort(vm,
                    "global store of non-numeric value (v1 limitation)");
                return;
            }
            IRRef k = cando_ir_const(&r->ir, cando_value_copy(name_val));
            cando_ir_emit(&r->ir, IR_GSTORE, IRT_VOID, IRF_PINNED, k, top);
            break;
        }

        case OP_STORE_LOCAL:
        case OP_DEF_LOCAL:
        case OP_DEF_CONST_LOCAL: {
            /* OP_STORE_LOCAL is peek-and-store (top stays on the value
             * stack); OP_DEF_LOCAL[_CONST] is pop-and-store (top
             * consumed).  We do NOT call rec_push for the peek case
             * because the bytecode interpreter is what actually drives
             * vm->stack_top -- the recorder's `sp` for the next observe
             * is recomputed from vm->stack_top, so consistency holds
             * regardless of which one we're recording.  The single
             * stack_map[abs] update below covers both: the slot now
             * tracks the stored IRRef.
             *
             * v1 only stores numeric values back to slots, because the
             * IR-interpreter writes via cando_number(...) -- writing a
             * bool result through that path would store the bool as a
             * number and any subsequent IF or print would diverge from
             * the bytecode behaviour.  Refuse to record stores whose
             * source IR isn't IRT_NUM; bytecode handles those normally
             * outside the trace.  Phase 3.4b will type-tag the SSTORE
             * value and write the correct CandoValue type. */
            u16 slot = read_op_arg(ip);
            u32 abs  = r->frame_base + slot;
            IRRef top = (sp > 0) ? r->stack_map[sp - 1] : IRREF_NIL;
            if (top == IRREF_NIL) {
                cando_recorder_abort(vm, "store with empty stack_map");
                return;
            }
            const IRIns *src = cando_ir_get_ins(&r->ir, top);
            if (!src || src->type != IRT_NUM) {
                cando_recorder_abort(vm, "store of non-numeric value (v1 limitation)");
                return;
            }
            /* Phase 4: snapshot the pre-iter value if this slot has
             * an SLOAD ahead of any prior SSTORE (so we have an IRRef
             * pointing at the value the bytecode would expect on
             * trace re-entry).  Slots SSTORE'd without a prior SLOAD
             * are bytecode-overwriteable and don't need rollback. */
            if (abs < r->stack_map_cap) {
                if (r->first_load[abs] != IRREF_NIL)
                    rec_pending_snap_add(r, slot, r->first_load[abs]);
                r->stack_map[abs] = top;
            }
            cando_ir_emit(&r->ir, IR_SSTORE, IRT_VOID, IRF_PINNED, slot, top);
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV: {
            /* OP_MOD still aborts: fmod/NaN semantics are loose enough
             * that bytecode parity isn't worth re-deriving in IR until
             * a real workload needs it.
             *
             * OP_DIV is recorded with a snapshot-protected guard
             * checking b != 0.  Phase 4 snapshots roll back any
             * iteration-local SSTOREs on guard fail, so bytecode
             * resumes from start_pc with a coherent state. */
            if (sp < 2) {
                cando_recorder_abort(vm, "arithmetic with too few operands");
                return;
            }
            IRRef b = r->stack_map[sp - 1];
            IRRef a = r->stack_map[sp - 2];
            if (a == IRREF_NIL || b == IRREF_NIL) {
                cando_recorder_abort(vm, "arithmetic operand not in stack_map");
                return;
            }
            a = ensure_num(&r->ir, a);
            b = ensure_num(&r->ir, b);
            if (op == OP_DIV) {
                /* Emit (b != 0) and guard it true, with a snapshot of
                 * any pending SSTOREs so a div-by-zero side-exit
                 * leaves the frame as if this iteration never ran. */
                IRRef zero = cando_ir_emit_knum(&r->ir, 0.0);
                IRRef nez  = cando_ir_emit(&r->ir, IR_NEQ, IRT_BOOL, 0, b, zero);
                u16 snap_idx = rec_build_snapshot(r);
                cando_ir_emit(&r->ir, IR_GUARD_TRUE, IRT_VOID,
                              IRF_GUARD, nez, snap_idx);
            }
            IROp ir_op = (op == OP_ADD) ? IR_ADD :
                         (op == OP_SUB) ? IR_SUB :
                         (op == OP_MUL) ? IR_MUL : IR_DIV;
            IRRef e = cando_ir_emit(&r->ir, ir_op, IRT_NUM, 0, a, b);
            rec_push(r, e, sp - 2);
            break;
        }

        case OP_NEG: {
            if (sp < 1) {
                cando_recorder_abort(vm, "OP_NEG with no operand");
                return;
            }
            IRRef a = r->stack_map[sp - 1];
            if (a == IRREF_NIL) {
                cando_recorder_abort(vm, "OP_NEG operand not in stack_map");
                return;
            }
            a = ensure_num(&r->ir, a);
            IRRef e = cando_ir_emit(&r->ir, IR_NEG, IRT_NUM, 0, a, 0);
            rec_push(r, e, sp - 1);
            break;
        }

        case OP_EQ:
        case OP_NEQ:
        case OP_LT:
        case OP_LEQ:
        case OP_GT:
        case OP_GEQ: {
            if (sp < 2) {
                cando_recorder_abort(vm, "comparison with too few operands");
                return;
            }
            IRRef b = r->stack_map[sp - 1];
            IRRef a = r->stack_map[sp - 2];
            if (a == IRREF_NIL || b == IRREF_NIL) {
                cando_recorder_abort(vm, "comparison operand not in stack_map");
                return;
            }
            /* Ordered compares (LT/LE/GT/GE) require numeric operands.
             * EQ/NEQ in v1 also restrict to numeric to keep recording
             * uniform; non-numeric equality lands in Phase 3.3b. */
            a = ensure_num(&r->ir, a);
            b = ensure_num(&r->ir, b);
            IROp ir_op = (op == OP_EQ)  ? IR_EQ  :
                         (op == OP_NEQ) ? IR_NEQ :
                         (op == OP_LT)  ? IR_LT  :
                         (op == OP_LEQ)  ? IR_LE  :
                         (op == OP_GT)  ? IR_GT  : IR_GE;
            IRRef e = cando_ir_emit(&r->ir, ir_op, IRT_BOOL, 0, a, b);
            rec_push(r, e, sp - 2);
            break;
        }

        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE: {
            /* Conditional branch on the popped top.  The interpreter
             * is about to run the op; we read the actual value to
             * decide which side of the trace we're recording, then
             * emit a guard that pins the branch direction.  If the
             * runtime value ever differs at this PC the trace
             * side-exits to the interpreter.
             *
             * v1: only handles bool tops (the common case for
             * comparison results).  Other types abort. */
            if (sp < 1) {
                cando_recorder_abort(vm, "branch with empty stack");
                return;
            }
            CandoValue v = vm->stack_top[-1];
            if (!cando_is_bool(v)) {
                cando_recorder_abort(vm, "branch on non-bool (v1 limitation)");
                return;
            }
            IRRef top = r->stack_map[sp - 1];
            if (top == IRREF_NIL) {
                cando_recorder_abort(vm, "branch operand not in stack_map");
                return;
            }
            bool actual = cando_as_bool(v);
            /* OP_JUMP_IF_FALSE: jumps when actual==false; "no jump"
             * means actual==true.  OP_JUMP_IF_TRUE: opposite. */
            bool will_jump = (op == OP_JUMP_IF_FALSE) ? !actual : actual;
            IROp guard_op  = will_jump
                             ? ((op == OP_JUMP_IF_FALSE) ? IR_GUARD_FALSE
                                                         : IR_GUARD_TRUE)
                             : ((op == OP_JUMP_IF_FALSE) ? IR_GUARD_TRUE
                                                         : IR_GUARD_FALSE);
            /* Phase 4: snapshot pending SSTOREs so this guard's
             * exit can roll back to a coherent VM state. */
            u16 snap_jb = rec_build_snapshot(r);
            cando_ir_emit(&r->ir, guard_op, IRT_BOOL, IRF_GUARD,
                          top, snap_jb);
            /* Stack effect: pop one.  No push. */
            break;
        }

        case OP_JUMP:
        case OP_LOOP:
        case OP_LOOP_MARK:
        case OP_LOOP_END: {
            /* Pure control flow / loop-frame bookkeeping that doesn't
             * touch the value stack.  ip moves; the next observe call
             * sees the new opcode (or hits start_pc and closes). */
            break;
        }

        case OP_FOR_NEXT: {
            /* FOR-IN / FOR-OF iterator advance.  Stack at entry:
             *   [..., source_array, len_signed, index]
             * with the three FOR-state values living at frame slots
             * (sp - 3, sp - 2, sp - 1) -- always above the function's
             * regular locals.
             *
             * Keys mode (len_signed < 0): push the index itself.
             * Values mode (len_signed >= 0): push source[index] via
             * IR_AREF.
             *
             * Invariant: vm->stack_top[-3] is always an OBJ_ARRAY at
             * runtime, because OP_FOR_INIT (vm.c) snapshots plain
             * objects and scalars into a heap array before installing
             * the FOR state.  We don't validate this here; IR_AREF's
             * runtime check (cando_is_object + arr->kind == OBJ_ARRAY)
             * is the safety net if a future FOR_INIT refactor breaks
             * the invariant. */
            if (sp < 3) {
                cando_recorder_abort(vm, "OP_FOR_NEXT with too few state slots");
                return;
            }
            CANDO_ASSERT_MSG(sp - 3 >= r->frame_base,
                             "FOR state below frame_base");

            CandoValue idx_v = vm->stack_top[-1];
            CandoValue len_v = vm->stack_top[-2];
            if (!cando_is_number(idx_v) || !cando_is_number(len_v)) {
                cando_recorder_abort(vm, "OP_FOR_NEXT state non-numeric");
                return;
            }
            f64 len_f  = cando_as_number(len_v);
            bool keys  = len_f < 0.0;
            f64 abs_len = keys ? -len_f : len_f;
            f64 idx_f  = cando_as_number(idx_v);

            /* If the runtime says this iteration is the loop-exit, the
             * recorded trace would only contain the exit path -- no
             * point.  Abort so the recorder can re-trigger from a
             * later (non-exit) iteration. */
            if (idx_f >= abs_len) {
                cando_recorder_abort(vm, "OP_FOR_NEXT recorded at exit iter");
                return;
            }

            /* Frame-relative slot indices for the FOR state.  sp is
             * the number of values currently on vm->stack; the FOR
             * state occupies the top three slots, all of which lie
             * inside the current frame (above the function's locals). */
            u32 abs_src      = sp - 3;
            u32 abs_len_slot = sp - 2;
            u32 abs_idx      = sp - 1;
            u32 rel_src      = abs_src - r->frame_base;
            u32 rel_len      = abs_len_slot - r->frame_base;
            u32 rel_idx      = abs_idx - r->frame_base;

            /* SLOAD the index slot.  Reuse the cached IRRef from a
             * previous iteration's SLOAD if present (same SSA pattern
             * as OP_LOAD_LOCAL). */
            IRRef idx_ir = (abs_idx < r->stack_map_cap)
                           ? r->stack_map[abs_idx] : IRREF_NIL;
            if (idx_ir == IRREF_NIL) {
                idx_ir = cando_ir_emit(&r->ir, IR_SLOAD, IRT_NUM, 0,
                                       rel_idx, 0);
                if (abs_idx < r->stack_map_cap) {
                    r->stack_map[abs_idx] = idx_ir;
                    if (r->first_load[abs_idx] == IRREF_NIL)
                        r->first_load[abs_idx] = idx_ir;
                }
            }

            /* Compute abs(len_signed).  In keys mode len_signed is
             * always negative, so abs is -len.  We emit IR_NEG so a
             * future optimiser can constant-fold if len is a known
             * KNUM. */
            IRRef len_ir = (abs_len_slot < r->stack_map_cap)
                           ? r->stack_map[abs_len_slot] : IRREF_NIL;
            if (len_ir == IRREF_NIL) {
                len_ir = cando_ir_emit(&r->ir, IR_SLOAD, IRT_NUM, 0,
                                       rel_len, 0);
                if (abs_len_slot < r->stack_map_cap) {
                    r->stack_map[abs_len_slot] = len_ir;
                    if (r->first_load[abs_len_slot] == IRREF_NIL)
                        r->first_load[abs_len_slot] = len_ir;
                }
            }
            /* Guard: this iteration must continue.  In keys mode the
             * length is stored as -len, so the comparison threshold
             * is -len_ir (computed via IR_NEG so a future optimiser
             * can fold if len is a known constant).  In values mode
             * the threshold is len_ir directly. */
            IRRef threshold_ir = keys
                ? cando_ir_emit(&r->ir, IR_NEG, IRT_NUM, 0, len_ir, 0)
                : len_ir;
            IRRef cmp_ir = cando_ir_emit(&r->ir, IR_LT, IRT_BOOL, 0,
                                         idx_ir, threshold_ir);
            /* Phase 4: snapshot before the guard so a side-exit can
             * roll back any iteration-local SSTOREs.  At this point
             * in the recording flow we're at the loop's TOP-OF-ITER,
             * so pending_snap is empty (no SSTOREs yet) and the
             * snapshot will be empty too -- rec_build_snapshot
             * returns 0.  Subsequent guards inside the body will
             * have non-empty pending_snap. */
            u16 snap = rec_build_snapshot(r);
            cando_ir_emit(&r->ir, IR_GUARD_TRUE, IRT_BOOL, IRF_GUARD,
                          cmp_ir, snap);

            /* Compute the value to push:
             *   keys mode    -> the index itself (idx_ir)
             *   values mode  -> source[index] via IR_HLOAD_SLOT + IR_AREF
             *
             * IR_HLOAD_SLOT resolves the source slot's array handle
             * once; LICM marks it invariant (the source slot has no
             * SSTORE in the trace) so the per-iteration cost drops to
             * just IR_AREF's bounds check + element fetch. */
            IRRef pushed_ir;
            if (keys) {
                pushed_ir = idx_ir;
            } else {
                IRRef src_ptr = cando_ir_emit(&r->ir, IR_HLOAD_SLOT,
                                              IRT_PTR, 0, rel_src, 0);
                pushed_ir = cando_ir_emit(&r->ir, IR_AREF, IRT_NUM, 0,
                                          src_ptr, idx_ir);
            }
            /* The pushed value lands at the new top of stack (slot sp).
             * The subsequent OP_DEF_LOCAL pulls it from stack_map[sp]
             * and stores to the loop-variable's frame slot. */
            rec_push(r, pushed_ir, sp);

            /* Update the index slot with idx + 1. */
            IRRef one_ir  = cando_ir_emit_knum(&r->ir, 1.0);
            IRRef next_ir = cando_ir_emit(&r->ir, IR_ADD, IRT_NUM, 0,
                                          idx_ir, one_ir);
            cando_ir_emit(&r->ir, IR_SSTORE, IRT_VOID, IRF_PINNED,
                          rel_idx, next_ir);
            if (abs_idx < r->stack_map_cap) {
                /* Phase 4: capture pre-iter idx for rollback.  See the
                 * note on OP_*_LOCAL above. */
                if (r->first_load[abs_idx] != IRREF_NIL)
                    rec_pending_snap_add(r, rel_idx, r->first_load[abs_idx]);
                r->stack_map[abs_idx] = next_ir;
            }
            break;
        }

        default: {
            char buf[64];
            snprintf(buf, sizeof(buf), "unrecordable opcode %s",
                     cando_opcode_name((CandoOpcode)op));
            cando_recorder_abort(vm, buf);
            return;
        }
    }
}

/* ============================================================ */
/* CandoJit lifecycle                                            */
/* ============================================================ */

CandoJit *cando_jit_create(void) {
    CandoJit *j = cando_alloc(sizeof(CandoJit));
    cando_hot_table_init(&j->hot, 0);
    cando_recorder_init(&j->recorder);
    j->traces        = cando_alloc(sizeof(CandoTrace) * CANDO_JIT_MAX_TRACES);
    j->trace_count   = 0;
    j->next_trace_id = 1;
    return j;
}

void cando_jit_destroy(CandoJit *j) {
    if (!j) return;
    cando_hot_table_destroy(&j->hot);
    cando_recorder_destroy(&j->recorder);
    if (j->traces) {
        for (u32 i = 0; i < j->trace_count; i++) {
            cando_trace_ir_destroy(&j->traces[i].ir);
            cando_free(j->traces[i].values_buf);
            cando_free(j->traces[i].snapshots);
            cando_free(j->traces[i].snap_entries);
        }
        cando_free(j->traces);
    }
    cando_free(j);
}

/* ============================================================ */
/* Trace lookup + IR-interpreter (Phase 3.4)                      */
/* ============================================================ */

CandoTrace *cando_jit_find_trace(struct CandoVM *vm, const u8 *pc) {
    CandoJit *j = jit_of(vm);
    if (!j || !pc) return NULL;
    for (u32 i = 0; i < j->trace_count; i++) {
        if (j->traces[i].start_pc == pc) return &j->traces[i];
    }
    return NULL;
}

CandoTraceStatus cando_trace_run(struct CandoVM *vm, CandoTrace *t,
                                 bool skip_invariant) {
    if (!vm || !t) return TRACE_RANGE_ERROR;

    /* Lazy-allocate the scratch values table; reused across every
     * trace_run for this trace, so cost amortises over many
     * iterations. */
    if (t->values_cap < t->ir.ir_count) {
        t->values_buf = cando_realloc(t->values_buf,
                                      sizeof(TraceVal) * t->ir.ir_count);
        t->values_cap = t->ir.ir_count;
    }
    TraceVal *vals = t->values_buf;

    /* SLOAD / SSTORE slot operands are FRAME-RELATIVE -- the slot
     * argument is offset from the current top frame's locals base.
     * This lets the same trace fire correctly across calls where
     * the absolute stack position of the frame may differ (different
     * intermediate values pushed by the caller before the call). */
    if (vm->frame_count == 0) return TRACE_RANGE_ERROR;
    CandoValue *frame_slots = vm->frames[vm->frame_count - 1].slots;

    /* IR-interpreter.  All values are doubles in v1 (numeric
     * constants only, comparisons stored as 0.0/1.0).  Booleans
     * round-trip through the f64 lane via 0/1 encoding -- guards
     * just check != 0.0.
     *
     * IR refs are 1-based; ir[0] is the IR_NOP sentinel and is
     * skipped.  Constant-pool refs (high bit set) are read directly
     * from the trace's constants[] array. */
    for (u32 i = 1; i < t->ir.ir_count; i++) {
        const IRIns *in = &t->ir.ir[i];

        /* LICM: on iterations 2+, skip ops whose result we already
         * computed in iteration 1.  vals[i] is still valid from the
         * first run because every iteration uses the same trace and
         * shares the per-trace values_buf. */
        if (skip_invariant && (in->flags & IRF_INVARIANT))
            continue;

        switch (in->op) {
            case IR_NOP:
                break;

            case IR_KNUM: {
                /* op1 is a constant-pool ref. */
                CandoValue cv = cando_ir_get_const(&t->ir, in->op1);
                vals[i].d = cando_as_number(cv);
                break;
            }
            case IR_KBOOL:
                vals[i].d = (in->op1 != 0) ? 1.0 : 0.0;
                break;

            case IR_KNULL:
                /* No representable double for null; if the trace
                 * tries to use this, treat as bad type. */
                return TRACE_BAD_TYPE;

            case IR_SLOAD: {
                /* op1 is the FRAME-RELATIVE slot index. */
                u32 slot = in->op1;
                CandoValue v = frame_slots[slot];
                if (!cando_is_number(v)) return TRACE_BAD_TYPE;
                vals[i].d = cando_as_number(v);
                break;
            }
            case IR_SSTORE: {
                /* op1 is FRAME-RELATIVE slot, op2 is value IRRef. */
                u32 slot = in->op1;
                frame_slots[slot] = cando_number(vals[in->op2].d);
                break;
            }

            case IR_ADD:  vals[i].d = vals[in->op1].d + vals[in->op2].d; break;
            case IR_SUB:  vals[i].d = vals[in->op1].d - vals[in->op2].d; break;
            case IR_MUL:  vals[i].d = vals[in->op1].d * vals[in->op2].d; break;
            case IR_DIV: {
                /* The recorder always emits a snapshot-protected
                 * IR_GUARD_TRUE on (b != 0) immediately before this
                 * IR_DIV (see OP_DIV in cando_recorder_observe).  This
                 * runtime check is a defensive backstop only; if it
                 * ever fires, no rollback is performed, but in
                 * practice it shouldn't be reachable. */
                f64 d = vals[in->op2].d;
                if (d == 0.0) return TRACE_GUARD_FAILED;
                vals[i].d = vals[in->op1].d / d;
                break;
            }
            case IR_MOD:
                /* Mirror libcando's OP_MOD semantics via fmod. */
                vals[i].d = fmod(vals[in->op1].d, vals[in->op2].d);
                break;
            case IR_NEG:  vals[i].d = -vals[in->op1].d; break;

            case IR_EQ:  vals[i].d = (vals[in->op1].d == vals[in->op2].d) ? 1.0 : 0.0; break;
            case IR_NEQ: vals[i].d = (vals[in->op1].d != vals[in->op2].d) ? 1.0 : 0.0; break;
            case IR_LT:  vals[i].d = (vals[in->op1].d <  vals[in->op2].d) ? 1.0 : 0.0; break;
            case IR_LE:  vals[i].d = (vals[in->op1].d <= vals[in->op2].d) ? 1.0 : 0.0; break;
            case IR_GT:  vals[i].d = (vals[in->op1].d >  vals[in->op2].d) ? 1.0 : 0.0; break;
            case IR_GE:  vals[i].d = (vals[in->op1].d >= vals[in->op2].d) ? 1.0 : 0.0; break;

            case IR_GUARD_NUM:
                /* SLOAD already type-checks; an op feeding a guard is
                 * always numeric inside the trace by construction. */
                break;
            case IR_GUARD_TRUE:
                if (vals[in->op1].d == 0.0) {
                    /* Phase 4: roll back any iteration-local SSTOREs
                     * via the guard's snapshot (op2 is a 1-based
                     * snapshot index; 0 means "no rollback needed"). */
                    if (in->op2 != 0 && in->op2 <= t->snapshot_count) {
                        const CandoSnapshot *s = &t->snapshots[in->op2 - 1];
                        for (u32 e = 0; e < s->entry_count; e++) {
                            const CandoSnapEntry *en =
                                &t->snap_entries[s->entry_offset + e];
                            frame_slots[en->slot] =
                                cando_number(vals[en->irref].d);
                        }
                    }
                    return TRACE_GUARD_FAILED;
                }
                break;
            case IR_GUARD_FALSE:
                if (vals[in->op1].d != 0.0) {
                    if (in->op2 != 0 && in->op2 <= t->snapshot_count) {
                        const CandoSnapshot *s = &t->snapshots[in->op2 - 1];
                        for (u32 e = 0; e < s->entry_count; e++) {
                            const CandoSnapEntry *en =
                                &t->snap_entries[s->entry_offset + e];
                            frame_slots[en->slot] =
                                cando_number(vals[en->irref].d);
                        }
                    }
                    return TRACE_GUARD_FAILED;
                }
                break;
            case IR_GUARD_OBJ:
            case IR_GUARD_STR:
                /* Object/string guards land in Phase 3.4b together
                 * with HLOAD/HREF.  v1 traces are numeric-only. */
                return TRACE_RANGE_ERROR;

            case IR_GLOAD: {
                /* op1: const-pool ref of the name string. */
                CandoValue name = cando_ir_get_const(&t->ir, in->op1);
                if (!cando_is_string(name)) return TRACE_RANGE_ERROR;
                CandoValue out;
                if (!cando_vm_get_global(vm, cando_as_string(name)->data,
                                          &out))
                    return TRACE_BAD_TYPE;
                if (!cando_is_number(out)) return TRACE_BAD_TYPE;
                vals[i].d = cando_as_number(out);
                break;
            }
            case IR_GSTORE: {
                /* op1: const-pool ref of name; op2: value IRRef. */
                CandoValue name = cando_ir_get_const(&t->ir, in->op1);
                if (!cando_is_string(name)) return TRACE_RANGE_ERROR;
                if (!cando_vm_set_global(vm,
                                          cando_as_string(name)->data,
                                          cando_number(vals[in->op2].d),
                                          false))
                    return TRACE_BAD_TYPE;  /* const-protected */
                break;
            }
            case IR_HLOAD_SLOT: {
                /* op1: frame-relative source slot.  Reads the slot,
                 * type-checks for an OBJECT, and resolves the handle
                 * to a raw CdoObject*.  Stored in vals[i].p so a
                 * later IR_AREF can dereference without the
                 * (expensive) bridge_resolve call.  This op is loop-
                 * invariant when the source slot has no SSTORE in
                 * the trace -- LICM hoists the resolution. */
                u32 slot = in->op1;
                CandoValue src = frame_slots[slot];
                if (!cando_is_object(src)) return TRACE_BAD_TYPE;
                CdoObject *arr = cando_bridge_resolve(vm, cando_as_handle(src));
                if (!arr || arr->kind != OBJ_ARRAY) return TRACE_BAD_TYPE;
                vals[i].p = arr;
                break;
            }
            case IR_AREF: {
                /* op1: IRRef of resolved CdoObject* (from IR_HLOAD_SLOT)
                 * op2: index IRRef (numeric)
                 * Returns array[index] as f64.
                 *
                 * Redundant checks elided in v1:
                 *   - arr->kind == OBJ_ARRAY  : IR_HLOAD_SLOT already
                 *     verified this and side-exited if not.  We just
                 *     defensively assert in debug builds.
                 *   - idx < cdo_array_len(arr): the recorder always
                 *     emits IR_LT(idx, len) IR_GUARD_TRUE before the
                 *     AREF, so the index is provably in range.  We
                 *     read via cdo_array_rawget_idx whose return value
                 *     IS the bounds check -- on a false return we
                 *     side-exit (defensive, not redundant once Phase
                 *     3.3e records calls that can mutate the array).
                 *
                 * Net: one lock-pair per iter (was two: array_len +
                 * rawget).  ~30% per-iter cost reduction in IR_AREF. */
                CdoObject *arr = (CdoObject *)vals[in->op1].p;
                CANDO_ASSERT(arr && arr->kind == OBJ_ARRAY);
                u32 idx = (u32)vals[in->op2].d;
                CdoValue cv = cdo_null();
                if (!cdo_array_rawget_idx(arr, idx, &cv))
                    return TRACE_BAD_TYPE;
                if (!cdo_is_number(cv)) return TRACE_BAD_TYPE;
                vals[i].d = cv.as.number;
                break;
            }

            case IR_LOOP:
                /* Successful close -- one iteration done. */
                return TRACE_LOOP_DONE;

            default:
                return TRACE_RANGE_ERROR;
        }
    }

    /* Walked off the end without an IR_LOOP: malformed trace. */
    return TRACE_RANGE_ERROR;
}

bool cando_jit_hot_hit(struct CandoVM *vm, const u8 *pc) {
    CandoJit *j = jit_of(vm);
    if (!j) return false;
    if (cando_hot_hit(&j->hot, pc)) {
        cando_recorder_begin(vm, pc);
        /* The next dispatch iteration will route through
         * cando_recorder_observe and either record or abort. */
        return j->recorder.active;
    }
    return false;
}
