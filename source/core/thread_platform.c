/*
 * thread_platform.c -- Platform thread/mutex/cond implementation.
 *
 * Must compile with gcc -std=c11.
 */

#include "thread_platform.h"
#include <stdlib.h>

/* =========================================================================
 * POSIX (Linux / macOS) implementation
 * ===================================================================== */
#if !defined(CANDO_PLATFORM_WINDOWS)

bool cando_os_thread_create(cando_thread_t *t, cando_thread_fn_t fn,
                             void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}

void cando_os_thread_detach(cando_thread_t t) {
    pthread_detach(t);
}

void cando_os_thread_join(cando_thread_t t) {
    pthread_join(t, NULL);
}

void cando_os_thread_sleep_ms(u32 ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000L);
    nanosleep(&ts, NULL);
}

void cando_os_mutex_init(cando_mutex_t *m) {
    pthread_mutex_init(m, NULL);
}

void cando_os_mutex_destroy(cando_mutex_t *m) {
    pthread_mutex_destroy(m);
}

void cando_os_mutex_lock(cando_mutex_t *m) {
    pthread_mutex_lock(m);
}

void cando_os_mutex_unlock(cando_mutex_t *m) {
    pthread_mutex_unlock(m);
}

void cando_os_cond_init(cando_cond_t *c) {
    pthread_cond_init(c, NULL);
}

void cando_os_cond_destroy(cando_cond_t *c) {
    pthread_cond_destroy(c);
}

void cando_os_cond_wait(cando_cond_t *c, cando_mutex_t *m) {
    pthread_cond_wait(c, m);
}

void cando_os_cond_signal(cando_cond_t *c) {
    pthread_cond_signal(c);
}

void cando_os_cond_broadcast(cando_cond_t *c) {
    pthread_cond_broadcast(c);
}

/* =========================================================================
 * Win32 implementation
 * ===================================================================== */
#else /* CANDO_PLATFORM_WINDOWS */

bool cando_os_thread_create(cando_thread_t *t, cando_thread_fn_t fn,
                             void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t != NULL;
}

void cando_os_thread_detach(cando_thread_t t) {
    CloseHandle(t);
}

void cando_os_thread_join(cando_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

void cando_os_thread_sleep_ms(u32 ms) {
    Sleep((DWORD)ms);
}

void cando_os_mutex_init(cando_mutex_t *m) {
    InitializeCriticalSection(m);
}

void cando_os_mutex_destroy(cando_mutex_t *m) {
    DeleteCriticalSection(m);
}

void cando_os_mutex_lock(cando_mutex_t *m) {
    EnterCriticalSection(m);
}

void cando_os_mutex_unlock(cando_mutex_t *m) {
    LeaveCriticalSection(m);
}

void cando_os_cond_init(cando_cond_t *c) {
    InitializeConditionVariable(c);
}

void cando_os_cond_destroy(cando_cond_t *c) {
    (void)c; /* Win32 condition variables have no destroy */
}

void cando_os_cond_wait(cando_cond_t *c, cando_mutex_t *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}

void cando_os_cond_signal(cando_cond_t *c) {
    WakeConditionVariable(c);
}

void cando_os_cond_broadcast(cando_cond_t *c) {
    WakeAllConditionVariable(c);
}

#endif /* CANDO_PLATFORM_WINDOWS */
