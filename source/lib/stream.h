/*
 * lib/stream.h -- Unified streaming subsystem for CanDo.
 *
 * A `Stream` is a vtable-backed handle for byte-oriented I/O.  Adapters wrap
 * concrete transports (in-memory buffer, file, TCP/TLS socket, HTTP body,
 * process pipe, thread channel) behind a single interface so any source can
 * be `:pipe()`d into any sink.
 *
 * Slot pool / instance pattern mirrors `socket.c` exactly: a global pool
 * holds the C-side state, the script sees a CdoObject with a hidden
 * `__stream_id` field and a meta table of native methods.
 *
 * This step (step 1) registers the global `stream` object with the
 * in-memory adapter only:
 *
 *   stream.memory(initialBytes?)        -- duplex in-memory buffer
 *
 *   s:read(maxLen)        -- string ("" on EOF)
 *   s:readAll()           -- drain to EOF
 *   s:write(data)         -- bytes consumed
 *   s:writeAll(data)      -- loops until done
 *   s:flush()             -- adapter-defined
 *   s:end()               -- half-close write side
 *   s:close()             -- full close (idempotent)
 *   s:isClosed() / :error() / :bytesIn() / :bytesOut() / :kind()
 *
 * Subsequent steps add file/socket/http/process adapters and the pipe
 * driver; the public types declared here are stable across those steps.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_STREAM_H
#define CANDO_LIB_STREAM_H

#include "../vm/vm.h"
#include "../object/object.h"
#include "../core/lock.h"

#include <stdatomic.h>

/* =========================================================================
 * Public registration
 * ===================================================================== */

CANDO_API void cando_lib_stream_register(CandoVM *vm);

/* =========================================================================
 * Capability flags and status codes
 * ===================================================================== */

typedef enum StreamCaps {
    STREAM_CAP_READABLE = 1u << 0,
    STREAM_CAP_WRITABLE = 1u << 1,
    STREAM_CAP_SEEKABLE = 1u << 2,
    STREAM_CAP_DUPLEX   = STREAM_CAP_READABLE | STREAM_CAP_WRITABLE,
} StreamCaps;

typedef enum StreamStatus {
    STREAM_OK    = 0,   /* op succeeded; *n_out has byte count                */
    STREAM_AGAIN = 1,   /* would-block; non-blocking sources only             */
    STREAM_EOF   = 2,   /* clean end-of-stream; *n_out may be > 0             */
    STREAM_ERR   = 3,   /* unrecoverable error; ctx.last_err carries detail   */
} StreamStatus;

/* =========================================================================
 * Adapter vtable
 *
 * Every adapter populates one of these.  read/write/destroy are required
 * for any practical stream; flush/end/seek/tell are optional (NULL means
 * "not supported" and the script call is a no-op or error).
 * ===================================================================== */

typedef struct StreamVTable {
    /* Read up to `cap` bytes into `out`.  *n_out receives the byte count.
     * STREAM_EOF may be returned with n_out == 0 to indicate clean EOF. */
    StreamStatus (*read)(void *ctx, u8 *out, usize cap, usize *n_out);

    /* Write up to `len` bytes from `buf`.  *n_out receives bytes consumed. */
    StreamStatus (*write)(void *ctx, const u8 *buf, usize len, usize *n_out);

    /* Optional: flush adapter-internal buffers to underlying transport. */
    StreamStatus (*flush)(void *ctx);

    /* Optional: half-close the write side. */
    StreamStatus (*end)(void *ctx);

    /* Free `ctx` and underlying handle.  Required.  Idempotent. */
    void         (*destroy)(void *ctx);

    /* Optional: seekable streams.  whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END. */
    StreamStatus (*seek)(void *ctx, i64 off, int whence);
    i64          (*tell)(void *ctx);

    const char *kind_name;          /* "memory", "file", "tcp", … */

    /* When true the common methods (read/write/etc.) do NOT acquire the
     * slot's R/W lock around the vtable call; the adapter is responsible
     * for its own concurrency.  Required for any adapter that may *block*
     * inside read/write (channel, full-duplex socket) — otherwise a blocked
     * reader would hold the lock against any writer and deadlock. */
    bool self_locked;
} StreamVTable;

/* =========================================================================
 * Slot
 *
 * Pool element.  `lock` serialises read/write across VMs; `closed` is the
 * one-shot lifecycle flag (set by close()).  `bytes_in`/`bytes_out` are
 * advisory counters readable from script for diagnostics.
 * ===================================================================== */

#define STREAM_MAX_INSTANCES 256

typedef struct StreamSlot {
    bool                in_use;
    StreamCaps          caps;
    const StreamVTable *vt;
    void               *ctx;
    CandoLockHeader     lock;
    _Atomic(bool)       closed;
    _Atomic(bool)       write_ended;
    _Atomic(u64)        bytes_in;
    _Atomic(u64)        bytes_out;
    char                last_err[128];
} StreamSlot;

/* =========================================================================
 * Pool API (used by every adapter, including those defined in other
 * library files in subsequent steps).
 * ===================================================================== */

/*
 * stream_pool_alloc -- reserve a slot, store vt+ctx+caps, zero counters.
 * Returns slot index in [0, STREAM_MAX_INSTANCES) or -1 on exhaustion.
 *
 * The slot takes ownership of `ctx`; on stream_pool_release the vtable's
 * destroy() is invoked.
 */
int  stream_pool_alloc(const StreamVTable *vt, void *ctx, StreamCaps caps);

/*
 * stream_pool_get -- index → StreamSlot*.  Returns NULL if `idx` is out of
 * range or the slot is no longer in use.
 */
StreamSlot *stream_pool_get(int idx);

/*
 * stream_pool_release -- mark `closed`, call vt->destroy(ctx), free the
 * slot for reuse.  Idempotent.
 */
void stream_pool_release(int idx);

/*
 * stream_resolve_receiver -- given a method receiver, return the underlying
 * StreamSlot.  Returns NULL if `receiver` is not a stream object.
 */
StreamSlot *stream_resolve_receiver(CandoVM *vm, CandoValue receiver);

/*
 * stream_create_instance -- build a CdoObject with hidden `__stream_id` and
 * meta-table `_meta.stream`, returning the script-visible value.
 */
CandoValue stream_create_instance(CandoVM *vm, int slot_idx);

/*
 * stream_set_error -- copy a message into the slot's last_err buffer; safe
 * to call from any thread.  Truncates to fit.
 */
void stream_set_error(StreamSlot *s, const char *msg);

/* =========================================================================
 * Convenience: build a read-only memory stream pre-filled with `data`.
 *
 * Used by adapters that already have the bytes in memory (e.g. an HTTP
 * client response body) so the caller can hand a uniform stream object
 * back to script even when the underlying transport doesn't support real
 * streaming.  The returned stream is already `:end()`-ed; the next read
 * past the data returns clean EOF.
 *
 * Pushes nothing onto the VM stack — the caller does that.  Returns the
 * CandoValue for the new stream, or cando_null() on pool exhaustion.
 * ===================================================================== */
CandoValue cando_stream_from_bytes(CandoVM *vm, const void *data, usize len);

/* =========================================================================
 * Pipe driver
 *
 * Drains `src` into `dst` in `chunk`-sized hops until src signals EOF (or
 * either side closes / errors).  Backpressure is automatic — `dst->write`
 * blocks the loop until the sink consumes.
 *
 * Each chunk-read and chunk-write brackets its own R/W lock acquire/release
 * so two concurrent pipes on disjoint endpoints cannot deadlock.
 *
 * Returns 0 on success, -1 on I/O error.  *copied (if non-NULL) receives
 * the total byte count.
 *
 * To run a pipe off-thread, scripts use the existing `thread { … }` syntax
 * — there is no async variant in C.
 * ===================================================================== */
int cando_stream_pipe(int src_idx, int dst_idx, usize chunk, u64 *copied);

#endif /* CANDO_LIB_STREAM_H */
