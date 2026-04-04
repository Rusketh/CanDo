/*
 * handle.c -- Global handle/indirection table implementation.
 */

#include "handle.h"

/* -----------------------------------------------------------------------
 * Free-list helpers
 *
 * When a slot is free its ptr field encodes the index of the next free
 * slot (via uintptr_t cast).  CANDO_INVALID_HANDLE terminates the list.
 * --------------------------------------------------------------------- */
CANDO_INLINE u32 slot_next_free(const CandoHandleSlot *s) {
    return (u32)(uptr)s->ptr;
}

CANDO_INLINE void slot_set_next_free(CandoHandleSlot *s, u32 next) {
    s->ptr = (void *)(uptr)next;
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

void cando_handle_table_init(CandoHandleTable *t, u32 initial_cap) {
    if (initial_cap == 0)
        initial_cap = CANDO_HANDLE_TABLE_INIT_CAP;

    cando_lock_init(&t->lock);
    t->slots    = cando_alloc(sizeof(CandoHandleSlot) * initial_cap);
    t->capacity = initial_cap;
    t->count    = 0;

    /* Build initial free list: 0 -> 1 -> ... -> capacity-1 -> INVALID */
    for (u32 i = 0; i < initial_cap - 1; i++) {
        t->slots[i].live   = 0;
        t->slots[i].marked = 0;
        slot_set_next_free(&t->slots[i], i + 1);
    }
    t->slots[initial_cap - 1].live   = 0;
    t->slots[initial_cap - 1].marked = 0;
    slot_set_next_free(&t->slots[initial_cap - 1], CANDO_INVALID_HANDLE);
    t->free_head = 0;
}

void cando_handle_table_destroy(CandoHandleTable *t) {
    cando_free(t->slots);
    t->slots     = NULL;
    t->capacity  = 0;
    t->count     = 0;
    t->free_head = CANDO_INVALID_HANDLE;
}

/* -----------------------------------------------------------------------
 * Growth — must be called with write lock already held
 * --------------------------------------------------------------------- */
static void handle_table_grow(CandoHandleTable *t) {
    u32 old_cap = t->capacity;
    u32 new_cap = old_cap * 2;

    t->slots = cando_realloc(t->slots, sizeof(CandoHandleSlot) * new_cap);

    /* Chain new slots into a fresh free list, terminating with the old
       free_head so the lists are joined. */
    for (u32 i = old_cap; i < new_cap - 1; i++) {
        t->slots[i].live   = 0;
        t->slots[i].marked = 0;
        slot_set_next_free(&t->slots[i], i + 1);
    }
    t->slots[new_cap - 1].live   = 0;
    t->slots[new_cap - 1].marked = 0;
    slot_set_next_free(&t->slots[new_cap - 1], t->free_head);
    t->free_head = old_cap;
    t->capacity  = new_cap;
}

/* -----------------------------------------------------------------------
 * Handle operations
 * --------------------------------------------------------------------- */

HandleIndex cando_handle_alloc(CandoHandleTable *t, void *ptr) {
    cando_lock_write_acquire(&t->lock);

    if (t->free_head == CANDO_INVALID_HANDLE)
        handle_table_grow(t);

    u32 idx      = t->free_head;
    t->free_head = slot_next_free(&t->slots[idx]);

    t->slots[idx].ptr    = ptr;
    t->slots[idx].live   = 1;
    t->slots[idx].marked = 0;
    t->count++;

    cando_lock_write_release(&t->lock);
    return (HandleIndex)idx;
}

void *cando_handle_get(CandoHandleTable *t, HandleIndex idx) {
    CANDO_ASSERT(idx < t->capacity);
    CANDO_ASSERT(t->slots[idx].live);

    cando_lock_read_acquire(&t->lock);
    void *result = t->slots[idx].ptr;
    cando_lock_read_release(&t->lock);
    return result;
}

void cando_handle_set(CandoHandleTable *t, HandleIndex idx, void *ptr) {
    CANDO_ASSERT(idx < t->capacity);
    CANDO_ASSERT(t->slots[idx].live);

    cando_lock_write_acquire(&t->lock);
    t->slots[idx].ptr = ptr;
    cando_lock_write_release(&t->lock);
}

void cando_handle_free(CandoHandleTable *t, HandleIndex idx) {
    CANDO_ASSERT(idx < t->capacity);

    cando_lock_write_acquire(&t->lock);
    CANDO_ASSERT(t->slots[idx].live);

    t->slots[idx].live   = 0;
    t->slots[idx].marked = 0;
    slot_set_next_free(&t->slots[idx], t->free_head);
    t->free_head = idx;
    t->count--;

    cando_lock_write_release(&t->lock);
}

/* -----------------------------------------------------------------------
 * GC support
 * --------------------------------------------------------------------- */

void cando_handle_mark(CandoHandleTable *t, HandleIndex idx) {
    CANDO_ASSERT(idx < t->capacity);
    CANDO_ASSERT(t->slots[idx].live);
    /* GC runs stop-the-world in this skeleton; no lock needed for the
       mark bit itself. */
    t->slots[idx].marked = 1;
}

void cando_handle_clear_marks(CandoHandleTable *t) {
    cando_lock_write_acquire(&t->lock);
    for (u32 i = 0; i < t->capacity; i++)
        t->slots[i].marked = 0;
    cando_lock_write_release(&t->lock);
}

bool cando_handle_is_marked(CandoHandleTable *t, HandleIndex idx) {
    CANDO_ASSERT(idx < t->capacity);
    CANDO_ASSERT(t->slots[idx].live);
    return t->slots[idx].marked != 0;
}
