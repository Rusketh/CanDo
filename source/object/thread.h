/*
 * thread.h -- CdoThread: OS thread object for the Cando threading runtime.
 *
 * A CdoThread is a heap-allocated object stored in the VM's handle table
 * (kind = OBJ_THREAD).  It is the return value of the `thread` expression
 * and the operand of `await`.
 *
 * Layout compatibility:
 *   CandoLockHeader MUST be at offset 0 and ObjectKind `kind` MUST be at
 *   offset 16, matching the layout of CdoObject so that VM code can read
 *   the `kind` field through a CdoObject* cast.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_THREAD_H
#define CDO_THREAD_H

#include "../core/common.h"
#include "../core/lock.h"
#include "../core/thread_platform.h"
#include "../core/value.h"
#include "object.h"       /* ObjectKind, OBJ_THREAD */

/* Maximum number of return values a thread body can produce. */
#define CDO_THREAD_MAX_RESULTS 8

/* -------------------------------------------------------------------------
 * CdoThreadState -- lifecycle state of a thread.
 * ---------------------------------------------------------------------- */
typedef enum {
    CDO_THREAD_PENDING  = 0,  /* created but not yet running              */
    CDO_THREAD_RUNNING  = 1,  /* OS thread is executing                   */
    CDO_THREAD_DONE     = 2,  /* finished normally; results are ready     */
    CDO_THREAD_ERROR    = 3,  /* finished with an unhandled error         */
    CDO_THREAD_CANCELLED = 4, /* cancelled before or during execution     */
} CdoThreadState;

/* -------------------------------------------------------------------------
 * CdoThread
 *
 * The first two fields MUST match the layout of CdoObject:
 *   offset  0: CandoLockHeader  (16 bytes)
 *   offset 16: ObjectKind kind  (1 byte)
 * This allows the VM to safely read kind through a (CdoObject*) cast.
 * ---------------------------------------------------------------------- */
typedef struct CdoThread {
    /* Layout-compatible prefix with CdoObject ----------------------------- */
    CandoLockHeader      lock;          /* auto-lock header — offset 0      */
    ObjectKind           kind;          /* OBJ_THREAD — offset 16           */

    /* Thread state -------------------------------------------------------- */
    _Atomic(CdoThreadState) state;

    /* The function value that this thread executes.
     * Stored as a CandoValue so the VM's value retention rules apply.     */
    CandoValue           fn_val;        /* OBJ_FUNCTION handle value        */

    /* Return values written by the thread; valid when state == DONE.      */
    CandoValue           results[CDO_THREAD_MAX_RESULTS];
    u32                  result_count;

    /* Error value if state == ERROR.                                       */
    CandoValue           error;

    /* Promise-style callbacks.  Set via thread.then / thread.catch.
     * Protected by done_mutex.  Null when not set.                        */
    CandoValue           then_fn;   /* called with results on DONE          */
    CandoValue           catch_fn;  /* called with error on ERROR           */

    /* Handle index of this thread in the VM handle table.  Set by
     * OP_THREAD immediately after allocation; used by thread.current().   */
    HandleIndex          handle_idx;

    /* Completion synchronisation — parent blocks here during `await`.     */
    cando_mutex_t        done_mutex;
    cando_cond_t         done_cond;

    /* OS thread handle.  Detached after spawn so resources are freed
     * automatically when the thread exits.                                */
    cando_thread_t       os_thread;
} CdoThread;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/*
 * cdo_thread_new -- allocate and initialise a CdoThread.
 *
 * `fn_val` is the OBJ_FUNCTION CandoValue to execute; it is retained.
 * State is set to CDO_THREAD_PENDING.  The OS thread is NOT started.
 */
CdoThread *cdo_thread_new(CandoValue fn_val);

/*
 * cdo_thread_destroy -- release all resources owned by the thread object.
 *
 * Must only be called after the thread has finished (state >= DONE).
 * Does NOT free the CdoThread pointer itself (caller must cando_free it).
 */
void cdo_thread_destroy(CdoThread *t);

/* -------------------------------------------------------------------------
 * State transitions
 * ---------------------------------------------------------------------- */

/*
 * cdo_thread_set_results -- called by the thread trampoline to store N
 * return values.  Transitions state RUNNING → DONE and signals waiters.
 */
void cdo_thread_set_results(CdoThread *t, CandoValue *vals, u32 count);

/*
 * cdo_thread_set_error -- called by the thread trampoline on unhandled
 * error.  Transitions state RUNNING → ERROR and signals waiters.
 */
void cdo_thread_set_error(CdoThread *t, CandoValue err_val);

/*
 * cdo_thread_wait -- block until state transitions to DONE, ERROR, or
 * CANCELLED.  Safe to call from any thread.
 */
void cdo_thread_wait(CdoThread *t);

/*
 * cdo_thread_is_done -- non-blocking poll: returns true if the thread has
 * finished (state is DONE, ERROR, or CANCELLED).
 */
bool cdo_thread_is_done(const CdoThread *t);

#endif /* CDO_THREAD_H */
