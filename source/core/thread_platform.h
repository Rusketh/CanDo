/*
 * thread_platform.h -- Thin platform abstraction for OS threads, mutexes,
 *                      and condition variables.
 *
 * Wraps pthreads on Linux/macOS and Win32 on Windows behind a uniform API
 * used by the Cando threading runtime.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_THREAD_PLATFORM_H
#define CANDO_THREAD_PLATFORM_H

#include "common.h"

/* -------------------------------------------------------------------------
 * Platform-specific includes
 * ---------------------------------------------------------------------- */
#if defined(CANDO_PLATFORM_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600  /* Vista+: required for CONDITION_VARIABLE */
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

/* =========================================================================
 * Type aliases
 * ===================================================================== */

#if defined(CANDO_PLATFORM_WINDOWS)
typedef HANDLE          cando_thread_t;
typedef CRITICAL_SECTION cando_mutex_t;
typedef CONDITION_VARIABLE cando_cond_t;
#else
typedef pthread_t       cando_thread_t;
typedef pthread_mutex_t cando_mutex_t;
typedef pthread_cond_t  cando_cond_t;
#endif

/* =========================================================================
 * Thread entry function type
 * ===================================================================== */
#if defined(CANDO_PLATFORM_WINDOWS)
typedef DWORD (WINAPI *cando_thread_fn_t)(LPVOID arg);
#  define CANDO_THREAD_RETURN DWORD WINAPI
#  define CANDO_THREAD_RETURN_VAL 0
#else
typedef void *(*cando_thread_fn_t)(void *arg);
#  define CANDO_THREAD_RETURN void *
#  define CANDO_THREAD_RETURN_VAL NULL
#endif

/* =========================================================================
 * Thread lifecycle
 * ===================================================================== */

/*
 * cando_os_thread_create -- spawn a new OS thread running fn(arg).
 * Returns true on success.
 */
bool cando_os_thread_create(cando_thread_t *t, cando_thread_fn_t fn,
                             void *arg);

/*
 * cando_os_thread_detach -- detach a thread so its resources are reclaimed
 * automatically when it exits.  Do not call join after detach.
 */
void cando_os_thread_detach(cando_thread_t t);

/*
 * cando_os_thread_join -- block until thread t exits.
 */
void cando_os_thread_join(cando_thread_t t);

/*
 * cando_os_thread_sleep_ms -- sleep the calling thread for `ms` milliseconds.
 */
void cando_os_thread_sleep_ms(u32 ms);

/* =========================================================================
 * Mutex
 * ===================================================================== */

void cando_os_mutex_init(cando_mutex_t *m);
void cando_os_mutex_destroy(cando_mutex_t *m);
void cando_os_mutex_lock(cando_mutex_t *m);
void cando_os_mutex_unlock(cando_mutex_t *m);

/* =========================================================================
 * Condition variable
 * ===================================================================== */

void cando_os_cond_init(cando_cond_t *c);
void cando_os_cond_destroy(cando_cond_t *c);

/*
 * cando_os_cond_wait -- atomically release mutex and wait for a signal.
 * Reacquires mutex before returning.
 */
void cando_os_cond_wait(cando_cond_t *c, cando_mutex_t *m);

/*
 * cando_os_cond_signal -- wake one thread waiting on c.
 */
void cando_os_cond_signal(cando_cond_t *c);

/*
 * cando_os_cond_broadcast -- wake all threads waiting on c.
 */
void cando_os_cond_broadcast(cando_cond_t *c);

#endif /* CANDO_THREAD_PLATFORM_H */
