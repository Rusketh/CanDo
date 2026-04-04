/*
 * memory.c -- Block allocator and mark-and-sweep GC skeleton.
 */

#include "memory.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Live-block registry helpers (gc_lock must be held by caller)
 * --------------------------------------------------------------------- */

static void live_register(CandoMemCtrl *mc, CandoBlockHeader *hdr) {
    if (mc->live_count == mc->live_cap) {
        mc->live_cap   *= 2;
        mc->live_blocks = cando_realloc(mc->live_blocks,
                              sizeof(CandoBlockHeader *) * mc->live_cap);
    }
    mc->live_blocks[mc->live_count++] = hdr;
}

/* Remove by swapping with the last entry — O(1), order not preserved. */
static void live_unregister(CandoMemCtrl *mc, CandoBlockHeader *hdr) {
    for (u32 i = 0; i < mc->live_count; i++) {
        if (mc->live_blocks[i] == hdr) {
            mc->live_blocks[i] = mc->live_blocks[--mc->live_count];
            return;
        }
    }
    CANDO_ASSERT_MSG(0, "live_unregister: block not found in registry");
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

void cando_memctrl_init(CandoMemCtrl *mc) {
    cando_handle_table_init(&mc->handles, 0);
    cando_lock_init(&mc->gc_lock);
    mc->live_cap    = 64;
    mc->live_count  = 0;
    mc->live_blocks = cando_alloc(sizeof(CandoBlockHeader *) * mc->live_cap);
}

void cando_memctrl_destroy(CandoMemCtrl *mc) {
    /* Free all remaining live blocks. */
    for (u32 i = 0; i < mc->live_count; i++)
        cando_free(mc->live_blocks[i]);

    cando_free(mc->live_blocks);
    mc->live_blocks = NULL;
    mc->live_count  = 0;
    mc->live_cap    = 0;

    cando_handle_table_destroy(&mc->handles);
}

/* -----------------------------------------------------------------------
 * Allocation
 * --------------------------------------------------------------------- */

HandleIndex cando_memctrl_alloc(CandoMemCtrl *mc, u32 user_size) {
    CANDO_ASSERT(user_size > 0);

    usize             total = sizeof(CandoBlockHeader) + user_size;
    CandoBlockHeader *hdr   = cando_alloc(total);
    memset(hdr, 0, total);
    cando_lock_init(&hdr->lock);
    hdr->user_size = user_size;

    /* Serialise handle allocation and registry update. */
    cando_lock_write_acquire(&mc->gc_lock);
    HandleIndex h = cando_handle_alloc(&mc->handles, hdr);
    hdr->handle   = h;
    live_register(mc, hdr);
    cando_lock_write_release(&mc->gc_lock);

    return h;
}

void *cando_memctrl_get_ptr(CandoMemCtrl *mc, HandleIndex h) {
    CandoBlockHeader *hdr = cando_handle_get(&mc->handles, h);
    return CANDO_BLOCK_PAYLOAD(hdr);
}

CandoBlockHeader *cando_memctrl_get_header(CandoMemCtrl *mc, HandleIndex h) {
    return cando_handle_get(&mc->handles, h);
}

void cando_memctrl_free(CandoMemCtrl *mc, HandleIndex h) {
    cando_lock_write_acquire(&mc->gc_lock);
    CandoBlockHeader *hdr = cando_handle_get(&mc->handles, h);
    live_unregister(mc, hdr);
    cando_handle_free(&mc->handles, h);
    cando_free(hdr);
    cando_lock_write_release(&mc->gc_lock);
}

/* -----------------------------------------------------------------------
 * GC
 * --------------------------------------------------------------------- */

void cando_gc_mark(CandoMemCtrl *mc, HandleIndex h) {
    if (h == CANDO_INVALID_HANDLE)
        return;
    cando_handle_mark(&mc->handles, h);
}

void cando_gc_sweep(CandoMemCtrl *mc) {
    cando_lock_write_acquire(&mc->gc_lock);

    u32 i = 0;
    while (i < mc->live_count) {
        CandoBlockHeader *hdr = mc->live_blocks[i];
        HandleIndex       h   = hdr->handle;

        if (!cando_handle_is_marked(&mc->handles, h)) {
            /* Not reachable — free it.  live_unregister swaps this slot
               with the last entry; re-check index i without incrementing. */
            live_unregister(mc, hdr);
            cando_handle_free(&mc->handles, h);
            cando_free(hdr);
        } else {
            i++;
        }
    }

    cando_lock_write_release(&mc->gc_lock);
}

void cando_gc_collect(CandoMemCtrl *mc, const HandleIndex *roots,
                      u32 n_roots) {
    cando_handle_clear_marks(&mc->handles);

    for (u32 i = 0; i < n_roots; i++)
        cando_gc_mark(mc, roots[i]);

    cando_gc_sweep(mc);
}
