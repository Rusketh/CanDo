/*
 * src/core/sync.h -- mutex / condition-variable / thread shim for the
 * forms module.
 *
 * libcando does not export its own threading primitives to extension
 * modules, so each binary module bundles its own.  Pulled out of
 * forms_module.c so other core TUs (events, slots, manager) can use
 * the same names without duplicating the macros.
 *
 * Naming (`fm_*`, `FM_*`) is preserved verbatim from the original
 * inlined block so existing call sites compile unchanged.
 */

#ifndef CANDO_FORMS_CORE_SYNC_H
#define CANDO_FORMS_CORE_SYNC_H

#if defined(CANDO_PLATFORM_WINDOWS) || defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION    fm_mutex_t;
   typedef CONDITION_VARIABLE  fm_cond_t;
   typedef HANDLE              fm_thread_t;
#  define FM_MUTEX_INIT(m)    InitializeCriticalSection(m)
#  define FM_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#  define FM_MUTEX_LOCK(m)    EnterCriticalSection(m)
#  define FM_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#  define FM_COND_INIT(c)     InitializeConditionVariable(c)
#  define FM_COND_DESTROY(c)  ((void)(c))
#  define FM_COND_WAIT(c,m)   SleepConditionVariableCS((c),(m),INFINITE)
#  define FM_COND_SIGNAL(c)   WakeConditionVariable(c)
#  define FM_COND_BROADCAST(c) WakeAllConditionVariable(c)
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <pthread.h>
   typedef pthread_mutex_t fm_mutex_t;
   typedef pthread_cond_t  fm_cond_t;
   typedef pthread_t       fm_thread_t;
#  define FM_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define FM_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define FM_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#  define FM_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#  define FM_COND_INIT(c)     pthread_cond_init((c), NULL)
#  define FM_COND_DESTROY(c)  pthread_cond_destroy(c)
#  define FM_COND_WAIT(c,m)   pthread_cond_wait((c),(m))
#  define FM_COND_SIGNAL(c)   pthread_cond_signal(c)
#  define FM_COND_BROADCAST(c) pthread_cond_broadcast(c)
#endif

#endif /* CANDO_FORMS_CORE_SYNC_H */
