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
 */
void *cando_handle_get(CandoHandleTable *t, HandleIndex idx);

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
