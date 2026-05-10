/*
 * handle.h -- Global handle/indirection table for the Cando GC.
 *
 * Objects are never referenced by raw pointer in script code.  Instead a
 * HandleIndex (u32) indexes into the global CandoHandleTable, whose slot
 * stores the current raw pointer.  When the GC relocates a block it only
 * needs to update that one slot; every script reference remains valid.
 *
 * HandleIndex and CANDO_INVALID_HANDLE are defined in value.h — do not
 * redefine them here.
 *
 * Thread-safety: structural changes (alloc / free / resize) are protected
 * by the table's embedded write lock.  cando_handle_get and
 * cando_handle_set use a read lock for the common path.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_HANDLE_H
#define CANDO_HANDLE_H

#include "common.h"
#include "lock.h"
#include "value.h"   /* HandleIndex, CANDO_INVALID_HANDLE */

/* -----------------------------------------------------------------------
 * CandoHandleSlot -- one entry in the indirection table
 * --------------------------------------------------------------------- */
typedef struct CandoHandleSlot {
    void *ptr;      /* pointer to the heap object; encodes free-list next
                       index when live == 0 (cast via uintptr_t)          */
    u8    live;     /* 1 = slot is in use, 0 = slot is free               */
    u8    marked;   /* GC mark bit; set during the mark phase             */
    u8    _pad[2];
} CandoHandleSlot;

/* -----------------------------------------------------------------------
 * CandoHandleTable
 * --------------------------------------------------------------------- */
#define CANDO_HANDLE_TABLE_INIT_CAP 256u

typedef struct CandoHandleTable {
    CandoLockHeader  lock;       /* write-lock for structural changes     */
    CandoHandleSlot *slots;      /* dynamically allocated slot array      */
    u32              capacity;   /* allocated length of slots[]           */
    u32              count;      /* number of live (in-use) handles       */
    u32              free_head;  /* free-list head; CANDO_INVALID_HANDLE
                                    when the free list is empty           */
} CandoHandleTable;

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

/*
 * cando_handle_table_init -- initialise with the given initial capacity.
 * Pass 0 to use CANDO_HANDLE_TABLE_INIT_CAP.
 */
void cando_handle_table_init(CandoHandleTable *t, u32 initial_cap);

/*
 * cando_handle_table_destroy -- free all internal memory.
 * The caller is responsible for freeing objects the slots point to.
 */
void cando_handle_table_destroy(CandoHandleTable *t);

/* -----------------------------------------------------------------------
 * Handle operations
 * --------------------------------------------------------------------- */

/*
 * cando_handle_alloc -- allocate a new slot pointing to `ptr`.
 * Returns the assigned HandleIndex.  The table grows automatically; this
 * call never returns CANDO_INVALID_HANDLE in practice (aborts on OOM).
 */
HandleIndex cando_handle_alloc(CandoHandleTable *t, void *ptr);

/*
 * cando_handle_get -- return the pointer stored at `idx`.
 * Asserts that idx is valid and live.
 *
 * Inlined here so the interpreter dispatch loop and the JIT-recorder
 * lookup helpers can resolve a handle without a function call.  The
 * read-lock acquire/release is retained because the slot array can be
 * relocated by cando_handle_alloc -> handle_table_grow on a parallel
 * thread; readers must observe a stable t->slots pointer.
 *
 * For JIT-emitted machine code that has already proven the handle is
 * live and the table generation hasn't changed, use CANDO_HANDLE_DEREF
 * below to skip the lock and bounds check entirely.
 */
CANDO_INLINE void *cando_handle_get(CandoHandleTable *t, HandleIndex idx) {
    CANDO_ASSERT(idx < t->capacity);
    CANDO_ASSERT(t->slots[idx].live);

    cando_lock_read_acquire(&t->lock);
    void *result = t->slots[idx].ptr;
    cando_lock_read_release(&t->lock);
    return result;
}

/*
 * CANDO_HANDLE_DEREF -- single-load handle dereference.
 *
 * Reads the slot pointer with no lock, no bounds check, no liveness
 * check.  Intended for JIT-emitted code that has already verified
 * (a) the handle index is in range and (b) no concurrent
 * cando_handle_alloc is racing with the read.  See docs/jit-plan.md
 * §4.3 / §9.9 for the trace-entry generation guard that makes this
 * safe inside compiled traces.
 *
 * Don't use this from interpreter or library code -- prefer
 * cando_handle_get there.
 */
#define CANDO_HANDLE_DEREF(t, h)  ((t)->slots[(h)].ptr)

/*
 * cando_handle_set -- overwrite the pointer at `idx`.
 * Used by the GC to relocate blocks without invalidating existing handles.
 */
void cando_handle_set(CandoHandleTable *t, HandleIndex idx, void *ptr);

/*
 * cando_handle_free -- release the slot at `idx` back to the free pool.
 * Does NOT free the object the slot points to.
 */
void cando_handle_free(CandoHandleTable *t, HandleIndex idx);

/* -----------------------------------------------------------------------
 * GC support
 * --------------------------------------------------------------------- */

/* cando_handle_mark -- set the GC mark bit on `idx`. */
void cando_handle_mark(CandoHandleTable *t, HandleIndex idx);

/* cando_handle_clear_marks -- reset all mark bits (start of a GC cycle). */
void cando_handle_clear_marks(CandoHandleTable *t);

/* cando_handle_is_marked -- true if idx's mark bit is set. */
bool cando_handle_is_marked(CandoHandleTable *t, HandleIndex idx);

#endif /* CANDO_HANDLE_H */
