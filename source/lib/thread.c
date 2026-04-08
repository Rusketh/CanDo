/*
 * lib/thread.c -- Native thread-management library for Cando.
 *
 * Exposes a global `thread` object with the following methods:
 *
 *   thread.sleep(ms)      -- sleep the calling thread for ms milliseconds
 *   thread.id()           -- return the calling thread's numeric ID
 *   thread.done(t)        -- return true if thread t has finished
 *   thread.join(t, ...)   -- block until all threads finish; return merged results
 *   thread.cancel(t)      -- mark thread t as cancelled (best-effort)
 *   thread.state(t)       -- return state string: "pending"|"running"|"done"|"error"|"cancelled"
 *   thread.error(t)       -- return the error value (or null if not errored)
 *   thread.current()      -- return the current thread handle, or null on main thread
 *   thread.then(t, fn)    -- register success callback; fires with return values
 *   thread.catch(t, fn)   -- register error callback; fires with the error value
 *
 * Must compile with gcc -std=c11.
 */

#include "thread.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/thread.h"
#include "../core/lock.h"
#include "../core/thread_platform.h"

#include <stdatomic.h>

/* =========================================================================
 * Helpers
 * ======================================================================= */

/*
 * resolve_thread -- extract a CdoThread* from args[idx].
 * Returns NULL if the argument is missing or not an OBJ_THREAD handle.
 */
static CdoThread *resolve_thread(CandoVM *vm, CandoValue *args, int argc,
                                 int idx)
{
    if (idx >= argc || !cando_is_object(args[idx]))
        return NULL;

    CdoObject *obj = cando_bridge_resolve(vm, args[idx].as.handle);
    if (!obj || obj->kind != OBJ_THREAD)
        return NULL;

    return (CdoThread *)obj;
}

/* =========================================================================
 * thread.sleep(ms) → null
 *
 * Suspends the calling thread for ms milliseconds.  Non-integer values are
 * truncated.  Negative values are treated as zero.
 * ======================================================================= */

static int thread_sleep(CandoVM *vm, int argc, CandoValue *args)
{
    f64 ms_f = libutil_arg_num_at(args, argc, 0, 0.0);
    u32 ms   = (ms_f > 0.0) ? (u32)ms_f : 0u;
    cando_os_thread_sleep_ms(ms);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * thread.id() → number
 *
 * Returns a non-zero numeric identifier for the calling thread.
 * ======================================================================= */

static int thread_id(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    u64 tid = cando_thread_id();
    cando_vm_push(vm, cando_number((f64)tid));
    return 1;
}

/* =========================================================================
 * thread.done(t) → bool
 *
 * Returns true if the thread has finished (state is DONE, ERROR, or
 * CANCELLED), false otherwise.  Does not block.
 * ======================================================================= */

static int thread_done(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    cando_vm_push(vm, cando_bool(cdo_thread_is_done(t)));
    return 1;
}

/* =========================================================================
 * thread.join(t, ...) → <return values...>
 * thread.join([t1, t2, ...]) → <return values...>
 *
 * Blocks until all provided threads finish.  Accepts either:
 *   - One or more thread handles as separate arguments: thread.join(t1, t2)
 *   - A single array of thread handles: thread.join([t1, t2])
 *
 * Return values from all threads are merged into one stack in argument order.
 * If a thread ended with an error or is not a valid thread, null is pushed
 * for that thread's slot.
 * ======================================================================= */

static int thread_join_one(CandoVM *vm, CdoThread *t) {
    if (!t) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cdo_thread_wait(t);
    CdoThreadState s = atomic_load(&t->state);
    if (s == CDO_THREAD_DONE) {
        for (u32 i = 0; i < t->result_count; i++)
            cando_vm_push(vm, t->results[i]);
        return (int)t->result_count;
    }
    /* ERROR or CANCELLED — push null */
    cando_vm_push(vm, cando_null());
    return 1;
}

static int thread_join(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc == 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    /* Check if the single argument is an array of threads. */
    if (argc == 1 && cando_is_object(args[0])) {
        CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
        if (obj && obj->kind == OBJ_ARRAY) {
            u32 len = cdo_array_len(obj);
            if (len == 0) {
                cando_vm_push(vm, cando_null());
                return 1;
            }
            int total = 0;
            for (u32 i = 0; i < len; i++) {
                CdoValue cv = cdo_null();
                cdo_array_rawget_idx(obj, i, &cv);
                CandoValue v = cando_bridge_to_cando(vm, cv);
                CdoThread *t = NULL;
                if (cando_is_object(v)) {
                    CdoObject *tobj = cando_bridge_resolve(vm, v.as.handle);
                    if (tobj && tobj->kind == OBJ_THREAD)
                        t = (CdoThread *)tobj;
                }
                total += thread_join_one(vm, t);
            }
            return total;
        }
    }

    /* Multiple thread arguments (or single non-array argument). */
    int total = 0;
    for (int i = 0; i < argc; i++) {
        CdoThread *t = resolve_thread(vm, args, argc, i);
        total += thread_join_one(vm, t);
    }
    return total;
}

/* =========================================================================
 * thread.cancel(t) → bool
 *
 * Requests cancellation of thread t.  This is a best-effort signal: the
 * thread is marked CANCELLED immediately if it is still PENDING or RUNNING,
 * but running code is not interrupted.  Returns true if the state was
 * successfully changed, false if the thread had already finished.
 * ======================================================================= */

static int thread_cancel(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }

    /* Attempt a CAS from PENDING or RUNNING → CANCELLED. */
    CdoThreadState expected;
    bool changed = false;

    expected = CDO_THREAD_PENDING;
    if (atomic_compare_exchange_strong(&t->state, &expected,
                                       CDO_THREAD_CANCELLED)) {
        changed = true;
    } else {
        expected = CDO_THREAD_RUNNING;
        if (atomic_compare_exchange_strong(&t->state, &expected,
                                           CDO_THREAD_CANCELLED)) {
            changed = true;
        }
    }

    if (changed) {
        /* Wake any thread blocked in cdo_thread_wait so it can observe the
         * new CANCELLED state and unblock. */
        cando_os_mutex_lock(&t->done_mutex);
        cando_os_cond_broadcast(&t->done_cond);
        cando_os_mutex_unlock(&t->done_mutex);
    }

    cando_vm_push(vm, cando_bool(changed));
    return 1;
}

/* =========================================================================
 * thread.state(t) → string
 *
 * Returns the lifecycle state of thread t as a string:
 *   "pending" | "running" | "done" | "error" | "cancelled"
 * Returns "null" if t is not a valid thread handle.
 * ======================================================================= */

static int thread_state(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t) {
        libutil_push_cstr(vm, "null");
        return 1;
    }

    const char *name;
    switch (atomic_load(&t->state)) {
        case CDO_THREAD_PENDING:   name = "pending";   break;
        case CDO_THREAD_RUNNING:   name = "running";   break;
        case CDO_THREAD_DONE:      name = "done";      break;
        case CDO_THREAD_ERROR:     name = "error";     break;
        case CDO_THREAD_CANCELLED: name = "cancelled"; break;
        default:                   name = "unknown";   break;
    }
    libutil_push_cstr(vm, name);
    return 1;
}

/* =========================================================================
 * thread.error(t) → value
 *
 * Returns the error value stored on thread t if its state is ERROR,
 * or null otherwise.
 * ======================================================================= */

static int thread_error(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t || atomic_load(&t->state) != CDO_THREAD_ERROR) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    cando_os_mutex_lock(&t->done_mutex);
    CandoValue err = cando_value_copy(t->error);
    cando_os_mutex_unlock(&t->done_mutex);

    cando_vm_push(vm, err);
    return 1;
}

/* =========================================================================
 * thread.current() → thread handle | null
 *
 * Returns the handle for the currently executing Cando thread, or null
 * when called from the main (non-spawned) thread.
 * ======================================================================= */

static int thread_current(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    CdoThread *t = cando_current_thread();
    if (!t || t->handle_idx == CANDO_INVALID_HANDLE) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cando_vm_push(vm, cando_object_value(t->handle_idx));
    return 1;
}

/* =========================================================================
 * thread.then(t, fn) → null
 *
 * Registers fn as a success callback on thread t.  When t finishes with
 * DONE, fn is called with t's return values as arguments.
 *
 * If t has already finished when this is called, fn is invoked immediately
 * (synchronously, re-entering the VM dispatch loop).
 * ======================================================================= */

static int thread_then(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t || argc < 2 || !cando_is_object(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue fn = args[1];

    cando_os_mutex_lock(&t->done_mutex);
    CdoThreadState s = atomic_load(&t->state);

    if (s == CDO_THREAD_DONE) {
        /* Already done — collect results then call fn outside the lock. */
        CandoValue results_copy[CDO_THREAD_MAX_RESULTS];
        u32 count = t->result_count;
        for (u32 i = 0; i < count; i++)
            results_copy[i] = cando_value_copy(t->results[i]);
        cando_os_mutex_unlock(&t->done_mutex);

        cando_vm_call_value(vm, fn, results_copy, count);
        for (u32 i = 0; i < count; i++)
            cando_value_release(results_copy[i]);
    } else if (s == CDO_THREAD_PENDING || s == CDO_THREAD_RUNNING) {
        /* Store callback; trampoline will fire it. */
        cando_value_release(t->then_fn);
        t->then_fn = cando_value_copy(fn);
        cando_os_mutex_unlock(&t->done_mutex);
    } else {
        /* ERROR or CANCELLED — then callbacks don't fire. */
        cando_os_mutex_unlock(&t->done_mutex);
    }

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * thread.catch(t, fn) → null
 *
 * Registers fn as an error callback on thread t.  When t finishes with
 * ERROR, fn is called with t's error value as its argument.
 *
 * If t has already errored when this is called, fn is invoked immediately.
 * ======================================================================= */

static int thread_catch(CandoVM *vm, int argc, CandoValue *args)
{
    CdoThread *t = resolve_thread(vm, args, argc, 0);
    if (!t || argc < 2 || !cando_is_object(args[1])) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue fn = args[1];

    cando_os_mutex_lock(&t->done_mutex);
    CdoThreadState s = atomic_load(&t->state);

    if (s == CDO_THREAD_ERROR) {
        /* Already errored — collect error then call fn outside the lock. */
        CandoValue err = cando_value_copy(t->error);
        cando_os_mutex_unlock(&t->done_mutex);

        cando_vm_call_value(vm, fn, &err, 1);
        cando_value_release(err);
    } else if (s == CDO_THREAD_PENDING || s == CDO_THREAD_RUNNING) {
        /* Store callback; trampoline will fire it. */
        cando_value_release(t->catch_fn);
        t->catch_fn = cando_value_copy(fn);
        cando_os_mutex_unlock(&t->done_mutex);
    } else {
        /* DONE or CANCELLED — catch callbacks don't fire. */
        cando_os_mutex_unlock(&t->done_mutex);
    }

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

void cando_lib_thread_register(CandoVM *vm)
{
    CandoValue thread_val = cando_bridge_new_object(vm);
    CdoObject *thread_obj = cando_bridge_resolve(vm, thread_val.as.handle);

    libutil_set_method(vm, thread_obj, "sleep",   thread_sleep);
    libutil_set_method(vm, thread_obj, "id",      thread_id);
    libutil_set_method(vm, thread_obj, "done",    thread_done);
    libutil_set_method(vm, thread_obj, "join",    thread_join);
    libutil_set_method(vm, thread_obj, "cancel",  thread_cancel);
    libutil_set_method(vm, thread_obj, "state",   thread_state);
    libutil_set_method(vm, thread_obj, "error",   thread_error);
    libutil_set_method(vm, thread_obj, "current", thread_current);
    libutil_set_method(vm, thread_obj, "then",    thread_then);
    libutil_set_method(vm, thread_obj, "catch",   thread_catch);

    cando_vm_set_global(vm, "thread", thread_val, true);
}
