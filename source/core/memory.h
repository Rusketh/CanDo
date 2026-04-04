/*
 * memory.h -- Block allocator and mark-and-sweep GC skeleton.
 *
 * The Memory Controller (CandoMemCtrl) owns:
 *   - the global CandoHandleTable
 *   - a registry of all live CandoBlockHeaders for GC traversal
 *
 * Every GC-managed object is prefixed with a CandoBlockHeader that
 * embeds a CandoLockHeader (so the auto-locking layer can protect
 * individual blocks) plus bookkeeping metadata.
 *
 * GC lifecycle (skeleton):
 *   1. cando_gc_collect(mc, roots, n_roots)
 *      a. cando_handle_clear_marks()   — unmark all handles
 *      b. cando_gc_mark() per root     — mark reachable handles
 *      c. cando_gc_sweep()             — free all unmarked blocks
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_MEMORY_H
#define CANDO_MEMORY_H

#include "common.h"
#include "lock.h"
#include "value.h"
#include "handle.h"

/* -----------------------------------------------------------------------
 * CandoBlockHeader -- prefix of every GC-managed heap block.
 *
 * Embed at the very beginning of a heap allocation.  A pointer to the
 * allocation can be cast to CandoBlockHeader* to reach the lock and
 * the handle that owns this block.
 * --------------------------------------------------------------------- */
typedef struct CandoBlockHeader {
    CandoLockHeader lock;       /* per-object auto-lock (16 bytes, first) */
    HandleIndex     handle;     /* the CandoHandleTable slot for this obj  */
    u32             user_size;  /* bytes of usable payload after header    */
} CandoBlockHeader;

/* Pointer to the user payload that immediately follows the header. */
#define CANDO_BLOCK_PAYLOAD(hdr) \
    ((void *)((CandoBlockHeader *)(hdr) + 1))

/* -----------------------------------------------------------------------
 * CandoMemCtrl -- the Memory Controller
 * --------------------------------------------------------------------- */
typedef struct CandoMemCtrl {
    CandoHandleTable   handles;       /* global handle/indirection table   */
    CandoLockHeader    gc_lock;       /* serialises GC collect cycles      */
    CandoBlockHeader **live_blocks;   /* registry of all live blocks       */
    u32                live_count;
    u32                live_cap;
} CandoMemCtrl;

/* -----------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------- */

void cando_memctrl_init(CandoMemCtrl *mc);
void cando_memctrl_destroy(CandoMemCtrl *mc);

/* -----------------------------------------------------------------------
 * Allocation
 * --------------------------------------------------------------------- */

/*
 * cando_memctrl_alloc -- allocate a new block with `user_size` bytes of
 * payload.  Payload is zero-initialised.  Returns the HandleIndex that
 * identifies this object.
 */
HandleIndex cando_memctrl_alloc(CandoMemCtrl *mc, u32 user_size);

/*
 * cando_memctrl_get_ptr -- resolve a handle to its payload pointer.
 * The pointer is valid until the next GC sweep cycle.
 */
void *cando_memctrl_get_ptr(CandoMemCtrl *mc, HandleIndex h);

/*
 * cando_memctrl_get_header -- resolve a handle to its block header.
 * Use this to access the per-object lock for manual locking.
 */
CandoBlockHeader *cando_memctrl_get_header(CandoMemCtrl *mc, HandleIndex h);

/*
 * cando_memctrl_free -- immediately free the block at `h`, removing it
 * from the live registry and releasing the handle slot.
 */
void cando_memctrl_free(CandoMemCtrl *mc, HandleIndex h);

/* -----------------------------------------------------------------------
 * GC
 * --------------------------------------------------------------------- */

/*
 * cando_gc_mark -- mark handle `h` as reachable.  In a full
 * implementation this would recursively trace object fields; in this
 * skeleton only the direct handle is marked.
 */
void cando_gc_mark(CandoMemCtrl *mc, HandleIndex h);

/*
 * cando_gc_sweep -- free every live block that was not marked since the
 * last call to cando_handle_clear_marks().
 */
void cando_gc_sweep(CandoMemCtrl *mc);

/*
 * cando_gc_collect -- full stop-the-world collection cycle:
 *   clear marks -> mark roots -> sweep.
 */
void cando_gc_collect(CandoMemCtrl *mc, const HandleIndex *roots,
                      u32 n_roots);

#endif /* CANDO_MEMORY_H */
