/*
 * lock.h -- Auto-locking primitives for the Cando value/object header.
 *
 * Every heap object carries a CandoLockHeader at its start.  The header
 * holds:
 *   - lock_id  : atomic u64  -- Thread ID of the exclusive-write holder,
 *                               or 0 when no exclusive lock is held.
 *   - readers  : atomic u32  -- Count of active shared-read holders.
 *
 * Locking semantics:
 *   Shared (read)  : Multiple threads may hold simultaneously.
 *                    Blocked while any exclusive lock is held.
 *   Exclusive (write): Only one thread at a time.
 *                    Blocked while any shared-read lock is held.
 *
 * LockID == 0 means "unlocked".  The current thread's ID is obtained via
 * cando_thread_id() which wraps the platform TID.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LOCK_H
#define CANDO_LOCK_H

#include "common.h"

/* -----------------------------------------------------------------------
 * Thread ID helper
 * --------------------------------------------------------------------- */

/*
 * cando_thread_id -- returns a non-zero identifier for the calling thread.
 * The value is stable for the lifetime of the thread.
 */
u64 cando_thread_id(void);

/* -----------------------------------------------------------------------
 * CandoLockHeader
 *
 * Embed at the very beginning of every heap-allocated object so that a
 * pointer to the object can be cast to CandoLockHeader*.
 * --------------------------------------------------------------------- */
typedef struct CandoLockHeader {
    _Atomic(u64) lock_id;   /* exclusive writer's thread ID, or 0         */
    _Atomic(u32) readers;   /* number of active shared-read holders        */
    _Atomic(u32) _pad;      /* alignment padding to 16 bytes               */
} CandoLockHeader;

CANDO_STATIC_ASSERT(sizeof(CandoLockHeader) == 16,
                    "CandoLockHeader must be 16 bytes");

/* -----------------------------------------------------------------------
 * Lock acquire / release
 * --------------------------------------------------------------------- */

/*
 * cando_lock_read_acquire -- acquire a shared (read) lock.
 *
 * Spins until no exclusive writer holds the lock.  Multiple readers may
 * proceed concurrently.
 */
void cando_lock_read_acquire(CandoLockHeader *hdr);

/*
 * cando_lock_read_release -- release a previously acquired shared lock.
 */
void cando_lock_read_release(CandoLockHeader *hdr);

/*
 * cando_lock_write_acquire -- acquire an exclusive (write) lock.
 *
 * Spins until all readers have released and no other writer holds the lock.
 * Supports re-entrance: the same thread may acquire the write lock again
 * without deadlocking (the lock_id will already match).
 */
void cando_lock_write_acquire(CandoLockHeader *hdr);

/*
 * cando_lock_write_release -- release the exclusive lock.
 *
 * Must be called from the same thread that called cando_lock_write_acquire.
 */
void cando_lock_write_release(CandoLockHeader *hdr);

/* -----------------------------------------------------------------------
 * RAII-style guard macros (GCC statement-expression extension)
 *
 * Usage:
 *   CANDO_WITH_READ_LOCK(hdr)  { ... }
 *   CANDO_WITH_WRITE_LOCK(hdr) { ... }
 *
 * These are convenience wrappers for the common acquire/release pattern.
 * They are NOT safe across longjmp or setjmp.
 * --------------------------------------------------------------------- */
#define CANDO_WITH_READ_LOCK(hdr) \
    for (int _rl = (cando_lock_read_acquire(hdr), 1); \
         _rl; \
         _rl = (cando_lock_read_release(hdr), 0))

#define CANDO_WITH_WRITE_LOCK(hdr) \
    for (int _wl = (cando_lock_write_acquire(hdr), 1); \
         _wl; \
         _wl = (cando_lock_write_release(hdr), 0))

/* -----------------------------------------------------------------------
 * Utility
 * --------------------------------------------------------------------- */

/*
 * cando_lock_init -- zero-initialize a lock header (unlocked state).
 */
CANDO_INLINE void cando_lock_init(CandoLockHeader *hdr) {
    atomic_store(&hdr->lock_id, 0);
    atomic_store(&hdr->readers, 0);
    atomic_store(&hdr->_pad,    0);
}

/*
 * cando_lock_is_write_held_by_me -- true if the calling thread owns the
 * exclusive lock.  Useful for assertions inside locked regions.
 */
CANDO_INLINE bool cando_lock_is_write_held_by_me(const CandoLockHeader *hdr) {
    return atomic_load(&hdr->lock_id) == cando_thread_id();
}

#endif /* CANDO_LOCK_H */
