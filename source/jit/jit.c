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
#include "../vm/opcodes.h"
#include "../vm/chunk.h"

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
    r->stack_map     = NULL;
    r->stack_map_cap = 0;
    r->active        = false;
}

/* Lazy-allocate the stack_map sized to the VM's CANDO_STACK_MAX.
 * Resizes in place if the VM ever grows its stack (it doesn't today,
 * but the recorder doesn't bake that in). */
static void rec_ensure_stack_map(CandoRecorder *r, u32 want) {
    if (want <= r->stack_map_cap) return;
    u32 nc = r->stack_map_cap ? r->stack_map_cap * 2 : 256;
    while (nc < want) nc *= 2;
    r->stack_map = cando_realloc(r->stack_map, sizeof(IRRef) * nc);
    /* Zero only the new tail; the active prefix is reset per begin(). */
    memset(r->stack_map + r->stack_map_cap, 0,
           sizeof(IRRef) * (nc - r->stack_map_cap));
    r->stack_map_cap = nc;
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
    memset(r->stack_map, 0, sizeof(IRRef) * r->stack_map_cap);
}

/* Finish a successfully closed trace: emit IR_LOOP, copy the IR into
 * the trace cache, deactivate.  Caller has confirmed ip == start_pc. */
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
                if (abs < r->stack_map_cap) r->stack_map[abs] = src;
            }
            rec_push(r, src, sp);
            break;
        }

        case OP_STORE_LOCAL:
        case OP_DEF_LOCAL:
        case OP_DEF_CONST_LOCAL: {
            /* peek-and-store (STORE_LOCAL) or pop-and-store (DEF_LOCAL):
             * mirror the stack effect on stack_map and emit IR_SSTORE.
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
            if (abs < r->stack_map_cap) r->stack_map[abs] = top;
            cando_ir_emit(&r->ir, IR_SSTORE, IRT_VOID, IRF_PINNED, slot, top);
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL: {
            /* DIV / MOD are intentionally NOT recorded in v1 because
             * they have side-effecting failure modes (OP_DIV raises
             * a runtime error on b==0; OP_MOD returns NaN silently).
             * The IR-interpreter would have to side-exit BEFORE any
             * preceding SSTORE commits, which requires snapshots
             * (Phase 3.4b).  Until then, abort traces that contain
             * DIV/MOD; bytecode handles them normally. */
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
            IROp ir_op = (op == OP_ADD) ? IR_ADD :
                         (op == OP_SUB) ? IR_SUB : IR_MUL;
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
            cando_ir_emit(&r->ir, guard_op, IRT_BOOL, IRF_GUARD, top, 0);
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

CandoTraceStatus cando_trace_run(struct CandoVM *vm, CandoTrace *t) {
    if (!vm || !t) return TRACE_RANGE_ERROR;

    /* Lazy-allocate the scratch values table; reused across every
     * trace_run for this trace, so cost amortises over many
     * iterations. */
    if (t->values_cap < t->ir.ir_count) {
        t->values_buf = cando_realloc(t->values_buf,
                                      sizeof(f64) * t->ir.ir_count);
        t->values_cap = t->ir.ir_count;
    }
    f64 *vals = t->values_buf;

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

        switch (in->op) {
            case IR_NOP:
                break;

            case IR_KNUM: {
                /* op1 is a constant-pool ref. */
                CandoValue cv = cando_ir_get_const(&t->ir, in->op1);
                vals[i] = cando_as_number(cv);
                break;
            }
            case IR_KBOOL:
                vals[i] = (in->op1 != 0) ? 1.0 : 0.0;
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
                vals[i] = cando_as_number(v);
                break;
            }
            case IR_SSTORE: {
                /* op1 is FRAME-RELATIVE slot, op2 is value IRRef. */
                u32 slot = in->op1;
                frame_slots[slot] = cando_number(vals[in->op2]);
                break;
            }

            case IR_ADD:  vals[i] = vals[in->op1] + vals[in->op2]; break;
            case IR_SUB:  vals[i] = vals[in->op1] - vals[in->op2]; break;
            case IR_MUL:  vals[i] = vals[in->op1] * vals[in->op2]; break;
            case IR_DIV: {
                f64 d = vals[in->op2];
                if (d == 0.0) return TRACE_GUARD_FAILED;  /* matches OP_DIV */
                vals[i] = vals[in->op1] / d;
                break;
            }
            case IR_MOD:
                /* Mirror libcando's OP_MOD semantics via fmod. */
                vals[i] = fmod(vals[in->op1], vals[in->op2]);
                break;
            case IR_NEG:  vals[i] = -vals[in->op1]; break;

            case IR_EQ:  vals[i] = (vals[in->op1] == vals[in->op2]) ? 1.0 : 0.0; break;
            case IR_NEQ: vals[i] = (vals[in->op1] != vals[in->op2]) ? 1.0 : 0.0; break;
            case IR_LT:  vals[i] = (vals[in->op1] <  vals[in->op2]) ? 1.0 : 0.0; break;
            case IR_LE:  vals[i] = (vals[in->op1] <= vals[in->op2]) ? 1.0 : 0.0; break;
            case IR_GT:  vals[i] = (vals[in->op1] >  vals[in->op2]) ? 1.0 : 0.0; break;
            case IR_GE:  vals[i] = (vals[in->op1] >= vals[in->op2]) ? 1.0 : 0.0; break;

            case IR_GUARD_NUM:
                /* SLOAD already type-checks; an op feeding a guard is
                 * always numeric inside the trace by construction. */
                break;
            case IR_GUARD_TRUE:
                if (vals[in->op1] == 0.0) return TRACE_GUARD_FAILED;
                break;
            case IR_GUARD_FALSE:
                if (vals[in->op1] != 0.0) return TRACE_GUARD_FAILED;
                break;
            case IR_GUARD_OBJ:
            case IR_GUARD_STR:
                /* Object/string guards land in Phase 3.4b together
                 * with HLOAD/HREF.  v1 traces are numeric-only. */
                return TRACE_RANGE_ERROR;

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
