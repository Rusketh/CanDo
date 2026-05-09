/*
 * jit/jit.h -- internal JIT module orchestrator.
 *
 * Phase 3.1: IR data plumbing (ir.h).
 * Phase 3.2: per-PC hot counter table (hot.h) + CandoJit wrapper.
 * Phase 3.3: real recorder body.  When the hot trigger fires, the
 *            dispatch loop's pre-execute hook (in vm.c's DISPATCH
 *            macro) calls cando_recorder_observe() on every opcode
 *            until the trace closes (back at start_pc) or aborts
 *            (unrecordable opcode, IR overflow, error).  The
 *            recorder shadows the interpreter's stack effects via
 *            its own SSA stack_map so it doesn't have to be hooked
 *            from inside every opcode handler.
 *
 * Recordable opcodes in v1 of Phase 3.3: OP_CONST (numbers only),
 * OP_LOAD_LOCAL / OP_STORE_LOCAL / OP_DEF_LOCAL, OP_ADD / OP_SUB /
 * OP_MUL, OP_LOOP (close).  Anything else aborts the trace.  More
 * opcodes are added incrementally in Phase 3.3b/c.
 *
 * Public C API for embedders (cando_jit_enable, cando_jit_get_stats,
 * etc.) lives in source/vm/vm.h.  This header is for internal users
 * (the dispatch loop and the future codegen).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_JIT_H
#define CANDO_JIT_JIT_H

#include "ir.h"
#include "hot.h"

/* Forward declarations: the recorder reads from CandoVM during
 * observe (frame depth, stack pointer, current chunk's constants).
 * Defining the full interaction here would create a circular include
 * with vm.h; observe takes (CandoVM*, ip) by pointer. */
struct CandoVM;
struct CandoChunk;

/* -----------------------------------------------------------------------
 * Tunables
 * --------------------------------------------------------------------- */
#define CANDO_JIT_MAX_IR_INS     4096u   /* trace length cap */
#define CANDO_JIT_MAX_TRACES     64u     /* completed traces stored per VM */

/* -----------------------------------------------------------------------
 * CandoTrace -- a finalised IR sequence the IR-interpreter executes
 * (Phase 3.4) and the future codegen compiles to mcode (Phase 6+).
 *
 * `values_buf` is a per-trace scratch array sized to ir.ir_count,
 * lazy-allocated by cando_trace_run on first execution.  Reusing the
 * buffer across runs avoids per-iteration alloca/malloc overhead.
 * --------------------------------------------------------------------- */
typedef struct CandoTrace {
    CandoTraceIR ir;          /* SSA instructions + constant pool */
    const u8    *start_pc;    /* head of the recorded loop */
    u32          id;          /* monotonic per-VM trace id */
    f64         *values_buf;  /* scratch table for cando_trace_run */
    u32          values_cap;
} CandoTrace;

/* -----------------------------------------------------------------------
 * Trace execution status (Phase 3.4).
 * --------------------------------------------------------------------- */
typedef enum {
    TRACE_LOOP_DONE     = 0,  /* hit IR_LOOP cleanly; caller may iterate */
    TRACE_GUARD_FAILED  = 1,  /* guard fired; bytecode resumes at start_pc */
    TRACE_BAD_TYPE      = 2,  /* SLOAD found a non-numeric value           */
    TRACE_RANGE_ERROR   = 3,  /* malformed IR -- should not happen         */
} CandoTraceStatus;

/* cando_jit_find_trace -- look up a compiled trace whose start_pc
 * matches `pc`.  Linear scan over CandoJit.traces[] (capped at
 * CANDO_JIT_MAX_TRACES = 64); cheap enough for the OP_LOOP hot path
 * when JIT is on, never called when JIT is off.  Returns NULL on
 * miss. */
const CandoTrace *cando_jit_find_trace(struct CandoVM *vm, const u8 *pc);

/* cando_trace_run -- execute one iteration of `trace` against the
 * VM's current stack state.  Reads SLOAD slots, computes IR values
 * in a per-trace scratch table, writes SSTORE slots back, and
 * returns the exit status.  Updates vm->jit_stats.trace_iters on
 * a clean LOOP_DONE exit. */
CandoTraceStatus cando_trace_run(struct CandoVM *vm, CandoTrace *trace);

/* -----------------------------------------------------------------------
 * CandoRecorder -- recording state.
 *
 * `active` flips to true on cando_recorder_begin (called from
 * cando_jit_hot_hit when the per-PC threshold trips) and back to
 * false on either cando_recorder_finish (loop closed cleanly) or
 * cando_recorder_abort (unrecordable opcode / overflow / error).
 *
 * `stack_map` is the recorder's shadow of the VM's value stack: each
 * slot holds the IRRef whose evaluation produces the current value
 * at that VM stack slot.  The recorder mirrors every recorded
 * opcode's stack effect on stack_map so subsequent opcodes can pull
 * operand IRRefs without re-emitting loads.
 *
 * `frame_base` is the starting slot of the recording frame, used to
 * resolve OP_LOAD_LOCAL / OP_DEF_LOCAL slot indices to absolute
 * stack positions.  If the recorded code calls into another
 * function, the recorder aborts (Phase 3.3b will add inlining).
 * --------------------------------------------------------------------- */
typedef struct CandoRecorder {
    bool                  active;
    const u8             *start_pc;
    CandoTraceIR          ir;
    /* stack_map[slot] = IRRef producing the value at vm->stack[slot].
     * Heap-allocated lazily on first cando_recorder_begin so jit.h
     * doesn't need to know CANDO_STACK_MAX from vm.h.  Sized to
     * match CANDO_STACK_MAX at allocation time. */
    IRRef                *stack_map;
    u32                   stack_map_cap;
    u32                   frame_base;     /* slot index, not pointer */
    u32                   frame_count_at_start;
    /* Stats (snapshotted into CandoJitStats at read time). */
    u32                   trace_starts;
    u32                   trace_aborts;
    u32                   traces_compiled;
    char                  last_abort[128];
} CandoRecorder;

void cando_recorder_init   (CandoRecorder *r);
void cando_recorder_destroy(CandoRecorder *r);

/* cando_recorder_begin -- arm the recorder rooted at `pc`.  No-op if
 * already active (nested triggers can't happen in single-threaded
 * recording -- the hot table auto-blacklists the trigger PC). */
void cando_recorder_begin  (struct CandoVM *vm, const u8 *pc);

/* cando_recorder_observe -- pre-execute hook called from the
 * dispatch loop's DISPATCH macro on every opcode while the recorder
 * is active.  `ip` points at the opcode byte about to execute.
 * Either records the opcode's IR + mirrors its stack effect, or
 * aborts the trace.  Detects loop-close (ip == start_pc with
 * non-empty IR) and finalises. */
void cando_recorder_observe(struct CandoVM *vm, const u8 *ip);

/* cando_recorder_abort -- record the abort reason and tear down
 * recording state.  Idempotent. */
void cando_recorder_abort  (struct CandoVM *vm, const char *reason);

/* -----------------------------------------------------------------------
 * CandoJit -- owns the hot table, the recorder, and the cache of
 * completed traces.
 *
 * Allocated lazily by cando_jit_enable so a process that never
 * enables the JIT pays nothing.
 * --------------------------------------------------------------------- */
typedef struct CandoJit {
    CandoHotTable  hot;
    CandoRecorder  recorder;
    CandoTrace    *traces;        /* heap array, capacity CANDO_JIT_MAX_TRACES */
    u32            trace_count;
    u32            next_trace_id;
} CandoJit;

CandoJit *cando_jit_create (void);
void      cando_jit_destroy(CandoJit *j);

/* cando_jit_hot_hit -- single entry point from the dispatch loop's
 * OP_LOOP handler.  Bumps the per-PC counter; on threshold,
 * activates the recorder (which the next DISPATCH iteration will
 * pick up via cando_recorder_observe). */
bool cando_jit_hot_hit(struct CandoVM *vm, const u8 *pc);

#endif /* CANDO_JIT_JIT_H */
