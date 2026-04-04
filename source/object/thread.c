/*
 * thread.c -- CdoThread implementation.
 *
 * Must compile with gcc -std=c11.
 */

#include "thread.h"
#include "../core/common.h"

/* =========================================================================
 * Lifecycle
 * ===================================================================== */

CdoThread *cdo_thread_new(CandoValue fn_val) {
    CdoThread *t = (CdoThread *)cando_alloc(sizeof(CdoThread));

    cando_lock_init(&t->lock);
    t->kind         = OBJ_THREAD;
    atomic_store(&t->state, CDO_THREAD_PENDING);
    t->fn_val       = cando_value_copy(fn_val);
    t->result_count = 0;
    t->error        = cando_null();
    t->then_fn      = cando_null();
    t->catch_fn     = cando_null();
    t->handle_idx   = CANDO_INVALID_HANDLE;

    for (u32 i = 0; i < CDO_THREAD_MAX_RESULTS; i++)
        t->results[i] = cando_null();

    cando_os_mutex_init(&t->done_mutex);
    cando_os_cond_init(&t->done_cond);

    return t;
}

void cdo_thread_destroy(CdoThread *t) {
    if (!t) return;

    cando_value_release(t->fn_val);
    cando_value_release(t->error);
    cando_value_release(t->then_fn);
    cando_value_release(t->catch_fn);
    for (u32 i = 0; i < CDO_THREAD_MAX_RESULTS; i++)
        cando_value_release(t->results[i]);

    cando_os_cond_destroy(&t->done_cond);
    cando_os_mutex_destroy(&t->done_mutex);
}

/* =========================================================================
 * State transitions
 * ===================================================================== */

void cdo_thread_set_results(CdoThread *t, CandoValue *vals, u32 count) {
    cando_os_mutex_lock(&t->done_mutex);

    t->result_count = count < CDO_THREAD_MAX_RESULTS
                      ? count : CDO_THREAD_MAX_RESULTS;
    for (u32 i = 0; i < t->result_count; i++)
        t->results[i] = cando_value_copy(vals[i]);

    atomic_store(&t->state, CDO_THREAD_DONE);
    cando_os_cond_broadcast(&t->done_cond);
    cando_os_mutex_unlock(&t->done_mutex);
}

void cdo_thread_set_error(CdoThread *t, CandoValue err_val) {
    cando_os_mutex_lock(&t->done_mutex);

    cando_value_release(t->error);
    t->error = cando_value_copy(err_val);

    atomic_store(&t->state, CDO_THREAD_ERROR);
    cando_os_cond_broadcast(&t->done_cond);
    cando_os_mutex_unlock(&t->done_mutex);
}

void cdo_thread_wait(CdoThread *t) {
    cando_os_mutex_lock(&t->done_mutex);
    while (atomic_load(&t->state) == CDO_THREAD_RUNNING ||
           atomic_load(&t->state) == CDO_THREAD_PENDING) {
        cando_os_cond_wait(&t->done_cond, &t->done_mutex);
    }
    cando_os_mutex_unlock(&t->done_mutex);
}

bool cdo_thread_is_done(const CdoThread *t) {
    CdoThreadState s = atomic_load(&t->state);
    return s == CDO_THREAD_DONE   ||
           s == CDO_THREAD_ERROR  ||
           s == CDO_THREAD_CANCELLED;
}
