/*
 * lock.c -- Spin-lock implementation for CandoLockHeader.
 *
 * Uses C11 <stdatomic.h> primitives.  The spin loops yield to the OS
 * scheduler via sched_yield() / SwitchToThread() after a short spin
 * count to avoid burning CPU on contended paths.
 */

/* _GNU_SOURCE required for syscall() on Linux */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "lock.h"

#ifdef CANDO_PLATFORM_WINDOWS
#  include <windows.h>
#  define cando_yield() SwitchToThread()
#elif defined(CANDO_PLATFORM_LINUX) || defined(CANDO_PLATFORM_MACOS)
#  include <sched.h>
#  define cando_yield() sched_yield()
#else
#  define cando_yield() ((void)0)
#endif

/* Number of tight-spin iterations before yielding. */
#define SPIN_COUNT 64

/* -----------------------------------------------------------------------
 * Thread ID
 * --------------------------------------------------------------------- */
#ifdef CANDO_PLATFORM_WINDOWS
#  include <windows.h>
u64 cando_thread_id(void) {
    return (u64)GetCurrentThreadId();
}
#elif defined(CANDO_PLATFORM_LINUX)
#  include <sys/types.h>
#  include <sys/syscall.h>
#  include <unistd.h>
u64 cando_thread_id(void) {
    return (u64)syscall(SYS_gettid);
}
#elif defined(CANDO_PLATFORM_MACOS)
#  include <pthread.h>
u64 cando_thread_id(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
}
#else
#  include <pthread.h>
u64 cando_thread_id(void) {
    return (u64)(uintptr_t)pthread_self();
}
#endif

/* -----------------------------------------------------------------------
 * Shared (read) lock
 * --------------------------------------------------------------------- */
void cando_lock_read_acquire(CandoLockHeader *hdr) {
    CANDO_ASSERT(hdr != NULL);

    int spins = 0;
    for (;;) {
        /* Wait until no exclusive writer is active. */
        while (atomic_load_explicit(&hdr->lock_id, memory_order_acquire) != 0) {
            if (++spins >= SPIN_COUNT) { spins = 0; cando_yield(); }
        }
        /* Announce our read interest. */
        atomic_fetch_add_explicit(&hdr->readers, 1, memory_order_acq_rel);
        /* Re-check that no writer sneaked in. */
        if (atomic_load_explicit(&hdr->lock_id, memory_order_acquire) == 0)
            return; /* acquired */
        /* Writer arrived; back off. */
        atomic_fetch_sub_explicit(&hdr->readers, 1, memory_order_release);
        if (++spins >= SPIN_COUNT) { spins = 0; cando_yield(); }
    }
}

void cando_lock_read_release(CandoLockHeader *hdr) {
    CANDO_ASSERT(hdr != NULL);
    u32 prev = atomic_fetch_sub_explicit(&hdr->readers, 1, memory_order_release);
    CANDO_ASSERT_MSG(prev > 0, "read_release called without matching acquire");
}

/* -----------------------------------------------------------------------
 * Exclusive (write) lock
 * --------------------------------------------------------------------- */
void cando_lock_write_acquire(CandoLockHeader *hdr) {
    CANDO_ASSERT(hdr != NULL);
    u64 me = cando_thread_id();

    /* Re-entrance: same thread already holds -- just increment depth. */
    if (atomic_load_explicit(&hdr->lock_id, memory_order_acquire) == me) {
        atomic_fetch_add_explicit(&hdr->write_depth, 1, memory_order_relaxed);
        return;
    }

    int spins = 0;
    u64 expected;

    /* CAS loop: claim the lock_id slot. */
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                &hdr->lock_id, &expected, me,
                memory_order_acq_rel, memory_order_relaxed))
            break;
        if (++spins >= SPIN_COUNT) { spins = 0; cando_yield(); }
    }

    /* Wait for all readers to drain. */
    while (atomic_load_explicit(&hdr->readers, memory_order_acquire) != 0) {
        if (++spins >= SPIN_COUNT) { spins = 0; cando_yield(); }
    }

    /* Outermost acquisition: depth starts at 1. */
    atomic_store_explicit(&hdr->write_depth, 1, memory_order_relaxed);
}

void cando_lock_write_release(CandoLockHeader *hdr) {
    CANDO_ASSERT(hdr != NULL);
    CANDO_ASSERT_MSG(cando_lock_is_write_held_by_me(hdr),
                     "write_release called by non-owner thread");

    /* Decrement depth; only fully release when depth reaches zero. */
    u32 prev = atomic_fetch_sub_explicit(&hdr->write_depth, 1,
                                         memory_order_acq_rel);
    if (prev == 1) {
        atomic_store_explicit(&hdr->lock_id, 0, memory_order_release);
    }
}
