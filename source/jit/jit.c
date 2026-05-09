/*
 * jit/jit.c -- CandoJit lifecycle + recorder stub.
 *
 * See jit.h for the surface.  The actual recorder body lands in
 * Phase 3.3; this file contains the trigger plumbing only.
 */

#include "jit.h"

#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Recorder stub
 * --------------------------------------------------------------------- */
void cando_recorder_init(CandoRecorder *r) {
    if (!r) return;
    r->trace_starts = 0;
    r->trace_aborts = 0;
    r->last_abort[0] = '\0';
}

void cando_recorder_destroy(CandoRecorder *r) {
    /* Nothing heap-allocated yet. */
    if (!r) return;
    r->trace_starts = 0;
    r->trace_aborts = 0;
    r->last_abort[0] = '\0';
}

void cando_recorder_abort(CandoRecorder *r, const char *reason) {
    if (!r) return;
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

void cando_recorder_begin(CandoRecorder *r, const u8 *pc) {
    if (!r) return;
    (void)pc;  /* Phase 3.3 will record at this PC */
    r->trace_starts++;
    /* The hot table auto-blacklisted this PC inside cando_hot_hit, so
     * we won't be re-triggered here.  Phase 3.3 introduces real
     * recording and replaces this stub. */
    cando_recorder_abort(r, "phase 3.3 recorder not yet implemented");
}

/* -----------------------------------------------------------------------
 * CandoJit lifecycle
 * --------------------------------------------------------------------- */
CandoJit *cando_jit_create(void) {
    CandoJit *j = cando_alloc(sizeof(CandoJit));
    cando_hot_table_init(&j->hot, 0);    /* default threshold */
    cando_recorder_init(&j->recorder);
    return j;
}

void cando_jit_destroy(CandoJit *j) {
    if (!j) return;
    cando_hot_table_destroy(&j->hot);
    cando_recorder_destroy(&j->recorder);
    cando_free(j);
}

/* -----------------------------------------------------------------------
 * Trigger
 * --------------------------------------------------------------------- */
bool cando_jit_hot_hit(CandoJit *j, const u8 *pc) {
    if (!j) return false;
    if (cando_hot_hit(&j->hot, pc)) {
        cando_recorder_begin(&j->recorder, pc);
        /* Phase 3.2 always aborts synchronously, so no trace exists
         * when we return.  Tell the caller false. */
        return false;
    }
    return false;
}
