/*
 * lib/thread.h -- Cando thread standard library.
 *
 * Exposes thread management functions as a global `thread` object:
 *
 *   thread.sleep(ms)      -- sleep current thread for ms milliseconds
 *   thread.id()           -- return current thread's numeric ID
 *   thread.done(t)        -- return true if thread t has finished
 *   thread.join(t)        -- block until thread t finishes; returns its results
 *   thread.cancel(t)      -- mark thread t as cancelled (best-effort)
 *   thread.state(t)       -- return state string: "pending"|"running"|"done"|"error"|"cancelled"
 *   thread.error(t)       -- return the error value if state is "error", else null
 *   thread.current()      -- return the current thread's handle, or null on the main thread
 *   thread.then(t, fn)    -- register fn as a success callback; fires with return values
 *   thread.catch(t, fn)   -- register fn as an error callback; fires with the error value
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_THREAD_H
#define CANDO_LIB_THREAD_H

#include "../vm/vm.h"

/*
 * cando_lib_thread_register -- register the thread library as a global
 * object named "thread" in the given VM.
 */
CANDO_API void cando_lib_thread_register(CandoVM *vm);

#endif /* CANDO_LIB_THREAD_H */
