/*
 * jit/jit.h -- internal JIT module orchestrator.
 *
 * Phase 3.1 published the IR data plumbing (ir.h).
 * Phase 3.2 adds:
 *   - per-PC hot counter table (hot.h),
 *   - the CandoJit wrapper that owns the table + recorder state,
 *   - a recorder stub: when the trigger fires the stub logs the PC
 *     (visible via --jit-stats counters) and immediately auto-
 *     blacklists.  Real recording arrives in Phase 3.3.
 *
 * Public C API for embedders (cando_jit_enable, cando_jit_get_stats,
 * etc.) lives in source/vm/vm.h alongside the rest of the embedding
 * surface.  This header is for internal users (the dispatch loop and
 * the future recorder/optimiser/codegen modules).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_JIT_H
#define CANDO_JIT_JIT_H

#include "ir.h"
#include "hot.h"

/* -----------------------------------------------------------------------
 * Recorder stub (Phase 3.2).
 *
 * Tracks how many times the trigger has fired and how many traces
 * aborted (both numbers are equal in Phase 3.2 because every trigger
 * aborts; they will diverge in Phase 3.3+).  No recording state is
 * carried between events yet -- the begin/abort pair is synchronous.
 * --------------------------------------------------------------------- */
typedef struct CandoRecorder {
    u32  trace_starts;       /* triggers that entered cando_recorder_begin */
    u32  trace_aborts;       /* of those, how many aborted (Phase 3.2: all)*/
    char last_abort[128];    /* most recent abort reason for diagnostics    */
} CandoRecorder;

void cando_recorder_init   (CandoRecorder *r);
void cando_recorder_destroy(CandoRecorder *r);

/* cando_recorder_begin -- invoked when cando_hot_hit returns true.
 * Phase 3.2 implementation: increments trace_starts, calls
 * cando_recorder_abort with reason "phase 3.3 unimplemented", returns.
 * Phase 3.3 will hold this open across many opcodes.                  */
void cando_recorder_begin  (CandoRecorder *r, const u8 *pc);

/* cando_recorder_abort -- record the abort and bump trace_aborts.
 * Caller is expected to have ensured the start PC is blacklisted
 * (cando_hot_hit auto-blacklists).                                    */
void cando_recorder_abort  (CandoRecorder *r, const char *reason);

/* -----------------------------------------------------------------------
 * CandoJit -- owns the per-PC hot table and the recorder state.
 *
 * Lives at CandoVM.jit (see vm.h).  Allocated lazily on the first
 * cando_jit_enable() so a process that never enables the JIT pays
 * nothing.
 * --------------------------------------------------------------------- */
typedef struct CandoJit {
    CandoHotTable  hot;
    CandoRecorder  recorder;
} CandoJit;

CandoJit *cando_jit_create (void);
void      cando_jit_destroy(CandoJit *j);

/* cando_jit_hot_hit -- single entry point from the dispatch loop.
 * Bumps the hot counter for `pc` and, when the threshold trips,
 * invokes the recorder.  Returns true if a trace was started (always
 * false in Phase 3.2 because the recorder aborts synchronously). */
bool cando_jit_hot_hit(CandoJit *j, const u8 *pc);

#endif /* CANDO_JIT_JIT_H */
