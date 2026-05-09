/*
 * jit/jit.h -- internal JIT module orchestrator.
 *
 * Pieces:
 *   ir.h  -- linear SSA IR data structures + construction.
 *   hot.h -- per-PC hot counter table.
 *   here  -- CandoJit container, recorder, IR-interpreter, snapshots,
 *            trace cache.
 *
 * Recording flow: on OP_LOOP backedges the dispatch loop calls
 * cando_jit_hot_hit; once the per-PC threshold trips, the recorder
 * activates.  The dispatch macro's pre-execute hook then routes every
 * subsequent opcode through cando_recorder_observe, which emits IR and
 * mirrors the opcode's stack effect onto its SSA stack_map.  The trace
 * closes when ip lands back at start_pc; aborts on unrecordable
 * opcode, IR overflow, frame boundary, or trace-cache full.
 *
 * Execution flow: cando_jit_find_trace looks up a compiled trace by
 * start_pc on each OP_LOOP; cando_trace_run executes one iteration via
 * the IR-interpreter and returns LOOP_DONE / GUARD_FAILED / BAD_TYPE.
 * The dispatch loop chains LOOP_DONE -> next iteration in a tight
 * inner loop.  See vm.c OP_LOOP.
 *
 * See docs/jit-plan.md for the recordable-opcode list and phased
 * roadmap.  Public C API for embedders (cando_jit_enable,
 * cando_jit_get_stats, etc.) lives in source/vm/vm.h.
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
#define CANDO_JIT_MAX_IR_INS      4096u  /* trace length cap */
#define CANDO_JIT_MAX_TRACES      64u    /* completed traces stored per VM */
#define CANDO_JIT_MAX_INLINE_DEPTH 4u    /* nested inlined CALLC depth cap */

/* -----------------------------------------------------------------------
 * CandoSnapshot -- captured state at a guard's bytecode position.
 *
 * Phase 4 of docs/jit-plan.md.  Each guard records the list of slots
 * the trace has SSTORE'd up to that point in the current iteration,
 * along with the IRRef of each slot's first SLOAD in this iteration.
 * On guard failure, cando_trace_run walks the snapshot, reads
 * vals[irref].d for each entry, and writes it back to the
 * frame slot -- effectively un-doing the trace's mid-iteration
 * SSTOREs so the bytecode interpreter can resume from start_pc with
 * a coherent VM state.
 *
 * Snapshots are owned by CandoTrace.  Entries live in a shared
 * arena (snap_entries) so the per-snapshot header stays small.
 * --------------------------------------------------------------------- */
/* Snapshot entry kind: SNAP_SLOT writes back to a frame slot;
 * SNAP_GLOBAL writes back to vm->globals via the trace's interned
 * name string.  Both restore numeric values; non-numeric stores
 * aren't recorded today. */
typedef enum {
    SNAP_SLOT   = 0,
    SNAP_GLOBAL = 1,
} CandoSnapKind;

typedef struct CandoSnapEntry {
    u8    kind;        /* CandoSnapKind */
    u32   key;         /* SNAP_SLOT: frame-relative slot.
                          SNAP_GLOBAL: trace IR const-pool index of
                          the name string (cando_ir_get_const). */
    IRRef irref;       /* IRRef whose vals[irref].d holds the pre-iter
                          value to write back. */
} CandoSnapEntry;

typedef struct CandoSnapshot {
    u32 entry_offset;  /* index into CandoTrace.snap_entries[]   */
    u32 entry_count;   /* how many entries belong to this snap   */
} CandoSnapshot;

/* -----------------------------------------------------------------------
 * TraceVal -- the IR-interpreter's per-IRRef scratch slot.
 *
 * 8 bytes wide.  Holds f64 for numeric IR ops, raw pointers for
 * IRT_PTR ops (e.g. IR_HLOAD_SLOT which caches a resolved
 * CdoObject*).  Unions cleanly type-pun in C11 -- no UB.
 * --------------------------------------------------------------------- */
typedef union TraceVal {
    f64       d;
    void     *p;
    uintptr_t u;
} TraceVal;

/* -----------------------------------------------------------------------
 * CandoTrace -- a finalised IR sequence the IR-interpreter executes
 * (Phase 3.4) and the future codegen compiles to mcode (Phase 6+).
 *
 * `values_buf` is a per-trace scratch array sized to ir.ir_count,
 * lazy-allocated by cando_trace_run on first execution.  Reusing the
 * buffer across runs avoids per-iteration alloca/malloc overhead.
 * --------------------------------------------------------------------- */
typedef struct CandoTrace {
    CandoTraceIR    ir;            /* SSA instructions + constant pool */
    const u8       *start_pc;      /* head of the recorded loop */
    u32             id;            /* monotonic per-VM trace id */
    u64             last_used;     /* approximate-LRU tick: bumped on
                                      every cando_jit_find_trace hit;
                                      smallest value is evicted first
                                      when the cache is full. */
    TraceVal       *values_buf;    /* scratch table for cando_trace_run */
    u32             values_cap;
    /* Snapshots (Phase 4) -- guards reference these by index via the
     * GUARD IR op's op2 field.  An op2 of 0 means "no snapshot" (the
     * guard predates Phase 4 or didn't need one). */
    CandoSnapshot  *snapshots;
    u32             snapshot_count;
    u32             snapshot_cap;
    CandoSnapEntry *snap_entries;
    u32             snap_entry_count;
    u32             snap_entry_cap;
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
 * miss.  Returns a non-const pointer because cando_trace_run mutates
 * the trace's lazy values_buf scratch on first invocation. */
CandoTrace *cando_jit_find_trace(struct CandoVM *vm, const u8 *pc);

/* cando_trace_run -- execute one iteration of `trace` against the
 * VM's current stack state.  Reads SLOAD slots, computes IR values
 * in a per-trace scratch table, writes SSTORE slots back, and
 * returns the exit status.  vm->jit_stats.trace_iters is incremented
 * by the dispatch-loop caller (vm.c OP_LOOP), not by this function.
 *
 * skip_invariant: false on the first iteration of an OP_LOOP entry
 * (computes everything, populating values_buf for both invariant and
 * variant ops).  true on subsequent iterations: ops marked
 * IRF_INVARIANT are skipped, reading their cached values from
 * values_buf.  Phase 5 LICM. */
CandoTraceStatus cando_trace_run(struct CandoVM *vm, CandoTrace *trace,
                                 bool skip_invariant);

/* -----------------------------------------------------------------------
 * Phase 4.2 stack-aux packing: one u32 per stack slot tracks
 * non-numeric values the recorder cares about.
 * --------------------------------------------------------------------- */
typedef enum {
    AUX_NONE          = 0,   /* numeric / use stack_map[abs] as IRRef    */
    AUX_OBJECT_GLOBAL = 1,   /* slot holds a global object; data = trace
                                IR const-pool index of the global name
                                (so we can re-look-it-up at trace-run
                                time if needed)                         */
    AUX_FAST_NATIVE   = 2,   /* slot holds a JIT fast-native function
                                pointer; data = vm->native_fns index    */
} CandoStackAuxKind;

#define CANDO_AUX_KIND(a)        ((CandoStackAuxKind)((a) & 0xFu))
#define CANDO_AUX_DATA(a)        ((u32)((a) >> 4))
#define CANDO_AUX_PACK(k, d)     ((u32)((u32)(k) | ((u32)(d) << 4)))

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
    /* Phase 4: per-slot tracking of the FIRST IR_SLOAD in the current
     * iteration.  Used by snapshot construction at guard emit time:
     * an SSTORE'd slot's pre-iter value is recoverable from
     * vals[first_load[slot]] inside cando_trace_run.
     * Shares stack_map_cap. */
    IRRef                *first_load;
    /* Phase 4.1: per-trace-IR-constant-index tracking of the FIRST
     * IR_GLOAD in the current iteration.  Parallels first_load[]
     * but keyed on the trace's interned name index.  Lazy-grown
     * alongside the IR's const_count. */
    IRRef                *first_load_global;
    u32                   first_load_global_cap;

    /* Phase 4.2: parallel-to-stack_map "aux" tag for slots holding
     * non-numeric values the recorder tracks (object globals, fast
     * native function pointers).  When aux[abs] is non-zero the
     * recorder treats stack_map[abs] as IRREF_NIL.  Layout:
     *   low 4 bits  = CandoStackAuxKind
     *   high 28 bits = kind-specific payload (e.g. const-pool name
     *   index for OBJECT_GLOBAL, native index for FAST_NATIVE).
     * Shares stack_map_cap. */
    u32                  *stack_aux;
    /* Pending snapshot entries -- accumulated as SSTOREs happen,
     * copied into the staging snapshot pool on each guard emit. */
    CandoSnapEntry       *pending_snap;
    u32                   pending_snap_count;
    u32                   pending_snap_cap;
    /* Staging snapshot pool -- grows as guards emit; transferred to
     * the new CandoTrace in cando_recorder_finish. */
    CandoSnapshot        *staging_snapshots;
    u32                   staging_snapshot_count;
    u32                   staging_snapshot_cap;
    CandoSnapEntry       *staging_snap_entries;
    u32                   staging_snap_entry_count;
    u32                   staging_snap_entry_cap;
    u32                   frame_base;     /* slot index, not pointer */
    u32                   frame_count_at_start;
    /* Phase 4.3: outer (recording-start) frame anchor.  All IR slot
     * operands are encoded relative to this so trace_run can read
     * them via vm->frames[top].slots[slot] regardless of whether the
     * trace inlined further calls.  outer_frame_base is set on
     * cando_recorder_begin and never changes during recording. */
    u32                   outer_frame_base;
    /* Inlined-call stack -- one entry per active CALLC inline.  v1
     * caps depth at CANDO_JIT_MAX_INLINE_DEPTH; deeper calls abort.
     * Each entry remembers the caller's frame_base so OP_RETURN can
     * restore it, and the absolute slot (callee_pos) where the call
     * happened so the return value lands at the right place in
     * stack_map. */
    u32                   call_depth;
    u32                   call_saved_frame_base[CANDO_JIT_MAX_INLINE_DEPTH];
    u32                   call_callee_pos      [CANDO_JIT_MAX_INLINE_DEPTH];
    /* PC offset of each currently-inlined callee.  Used by OP_CALL
     * to detect direct or indirect recursion (callee_pc already on
     * the inline stack) and abort with a recursion-specific reason
     * before bouncing against the depth cap. */
    u32                   call_callee_pc       [CANDO_JIT_MAX_INLINE_DEPTH];
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
    u64            next_use_tick;  /* monotonic counter for trace LRU */
    u32            traces_evicted; /* stat: how many times the cache evicted */
} CandoJit;

CandoJit *cando_jit_create (void);
void      cando_jit_destroy(CandoJit *j);

/* cando_jit_hot_hit -- single entry point from the dispatch loop's
 * OP_LOOP handler.  Bumps the per-PC counter; on threshold,
 * activates the recorder (which the next DISPATCH iteration will
 * pick up via cando_recorder_observe). */
bool cando_jit_hot_hit(struct CandoVM *vm, const u8 *pc);

#endif /* CANDO_JIT_JIT_H */
