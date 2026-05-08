/*
 * lib/stream.c -- Unified streaming subsystem (step 1: pool + mem adapter).
 *
 * Layered structure:
 *   1. Pool of StreamSlot, with mutex-guarded alloc/free (mirrors socket.c).
 *   2. Receiver helpers (look up StreamSlot from CanDo `__stream_id` field).
 *   3. Common method implementations (read/write/close/etc.) that dispatch
 *      to the slot's vtable under the slot's R/W lock.
 *   4. The in-memory adapter (`stream.memory`).
 *   5. Module registration and the `_meta.stream` table.
 *
 * Subsequent steps add file/socket/http/process adapters in their own
 * library files; they all call back into this module's pool/instance API.
 *
 * Must compile with gcc -std=c11.
 */

#include "stream.h"
#include "libutil.h"
#include "meta.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/string.h"
#include "../core/thread_platform.h"
#include "../core/lock.h"
#include "../core/common.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Pool
 * ===================================================================== */

static StreamSlot      g_stream_pool[STREAM_MAX_INSTANCES];
static cando_mutex_t   g_stream_pool_mutex;
static _Atomic(int)    g_stream_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_stream_pool_inited, &expected, 1)) {
        cando_os_mutex_init(&g_stream_pool_mutex);
        for (int i = 0; i < STREAM_MAX_INSTANCES; i++) {
            memset(&g_stream_pool[i], 0, sizeof(StreamSlot));
        }
    }
}

int stream_pool_alloc(const StreamVTable *vt, void *ctx, StreamCaps caps)
{
    if (!vt) return -1;
    ensure_pool_inited();
    cando_os_mutex_lock(&g_stream_pool_mutex);
    int idx = -1;
    for (int i = 0; i < STREAM_MAX_INSTANCES; i++) {
        if (!g_stream_pool[i].in_use) {
            memset(&g_stream_pool[i], 0, sizeof(StreamSlot));
            g_stream_pool[i].in_use = true;
            g_stream_pool[i].vt     = vt;
            g_stream_pool[i].ctx    = ctx;
            g_stream_pool[i].caps   = caps;
            cando_lock_init(&g_stream_pool[i].lock);
            atomic_store(&g_stream_pool[i].closed,      false);
            atomic_store(&g_stream_pool[i].write_ended, false);
            atomic_store(&g_stream_pool[i].bytes_in,    0);
            atomic_store(&g_stream_pool[i].bytes_out,   0);
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_stream_pool_mutex);
    return idx;
}

StreamSlot *stream_pool_get(int idx)
{
    if (idx < 0 || idx >= STREAM_MAX_INSTANCES) return NULL;
    if (!g_stream_pool[idx].in_use) return NULL;
    return &g_stream_pool[idx];
}

void stream_pool_release(int idx)
{
    if (idx < 0 || idx >= STREAM_MAX_INSTANCES) return;
    cando_os_mutex_lock(&g_stream_pool_mutex);
    StreamSlot *s = &g_stream_pool[idx];
    if (!s->in_use) {
        cando_os_mutex_unlock(&g_stream_pool_mutex);
        return;
    }
    /* Mark closed before destroying so any concurrent observer sees it.
     * The pool mutex serialises against further alloc/release; the per-slot
     * R/W lock is intentionally bypassed because destroy() must be free to
     * tear down the underlying handle without a deadlock. */
    atomic_store(&s->closed,      true);
    atomic_store(&s->write_ended, true);
    if (s->vt && s->vt->destroy) s->vt->destroy(s->ctx);
    s->ctx    = NULL;
    s->vt     = NULL;
    s->in_use = false;
    cando_os_mutex_unlock(&g_stream_pool_mutex);
}

void stream_set_error(StreamSlot *s, const char *msg)
{
    if (!s || !msg) return;
    /* No lock needed: writers race only on the same byte range; readers may
     * see a partial overwrite, which is acceptable for a diagnostic field. */
    snprintf(s->last_err, sizeof(s->last_err), "%s", msg);
}

/* =========================================================================
 * Field helpers (same shape as socket.c)
 * ===================================================================== */

static bool get_int_field(CdoObject *obj, const char *name, int *out)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = (int)v.as.number;
    return true;
}

static void set_num_field(CdoObject *obj, const char *name, f64 n)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(n), FIELD_NONE);
    cdo_string_release(key);
}

/* =========================================================================
 * Receiver resolution
 * ===================================================================== */

StreamSlot *stream_resolve_receiver(CandoVM *vm, CandoValue receiver)
{
    if (!cando_is_object(receiver)) return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(receiver));
    if (!obj) return NULL;
    int idx = -1;
    if (!get_int_field(obj, "__stream_id", &idx)) return NULL;
    return stream_pool_get(idx);
}

CandoValue stream_create_instance(CandoVM *vm, int slot_idx)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));
    set_num_field(obj, "__stream_id", (f64)slot_idx);
    cando_lib_meta_attach(vm, obj, "stream");
    return val;
}

/* =========================================================================
 * Common method implementations
 *
 * Every method takes the slot's write lock for the duration of the vtable
 * call.  This makes streams safe to share between parent and child VMs
 * without each adapter having to re-implement locking.
 * ===================================================================== */

static StreamSlot *receiver_or_throw(CandoVM *vm, int argc, CandoValue *args,
                                     const char *method)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    if (!s) {
        cando_vm_error(vm, "%s: invalid stream receiver", method);
        return NULL;
    }
    return s;
}

/*
 * Helpers that bracket a single vtable call with the slot's R/W lock,
 * honouring the adapter's `self_locked` opt-out.  Adapters that may block
 * inside read/write (channel, full-duplex socket) MUST set self_locked or
 * a blocked reader will hold the slot lock against any writer.
 */
static StreamStatus dispatch_read(StreamSlot *s, u8 *out, usize cap, usize *n_out)
{
    if (s->vt->self_locked) {
        return s->vt->read(s->ctx, out, cap, n_out);
    }
    StreamStatus st;
    CANDO_WITH_WRITE_LOCK(&s->lock) {
        st = s->vt->read(s->ctx, out, cap, n_out);
    }
    return st;
}

static StreamStatus dispatch_write(StreamSlot *s, const u8 *buf, usize len,
                                   usize *n_out)
{
    if (s->vt->self_locked) {
        return s->vt->write(s->ctx, buf, len, n_out);
    }
    StreamStatus st;
    CANDO_WITH_WRITE_LOCK(&s->lock) {
        st = s->vt->write(s->ctx, buf, len, n_out);
    }
    return st;
}

/* stream:read(maxLen) -> string ("" on EOF) */
static int stream_read_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:read");
    if (!s) return -1;
    if (!(s->caps & STREAM_CAP_READABLE)) {
        cando_vm_error(vm, "stream:read: not a readable stream");
        return -1;
    }
    if (atomic_load(&s->closed)) {
        libutil_push_str(vm, "", 0);
        return 1;
    }
    int maxlen = (int)libutil_arg_num_at(args, argc, 1, 4096);
    if (maxlen <= 0) {
        cando_vm_error(vm, "stream:read: maxLen must be positive");
        return -1;
    }
    if (maxlen > 16 * 1024 * 1024) maxlen = 16 * 1024 * 1024;

    u8 *buf = (u8 *)cando_alloc((usize)maxlen);
    usize n = 0;
    StreamStatus st = dispatch_read(s, buf, (usize)maxlen, &n);

    if (st == STREAM_ERR) {
        cando_free(buf);
        cando_vm_error(vm, "stream:read: %s",
                       s->last_err[0] ? s->last_err : "read failed");
        return -1;
    }
    /* STREAM_AGAIN with n == 0 surfaces as "" — matches socket recv contract. */
    atomic_fetch_add(&s->bytes_in, (u64)n);
    libutil_push_str(vm, (const char *)buf, (u32)n);
    cando_free(buf);
    return 1;
}

/* stream:readAll() -> string (drain to EOF) */
static int stream_readAll_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:readAll");
    if (!s) return -1;
    if (!(s->caps & STREAM_CAP_READABLE)) {
        cando_vm_error(vm, "stream:readAll: not a readable stream");
        return -1;
    }
    usize cap = 4096, len = 0;
    u8   *buf = (u8 *)cando_alloc(cap);
    while (!atomic_load(&s->closed)) {
        if (len + 4096 > cap) {
            usize ncap = cap * 2;
            buf = (u8 *)cando_realloc(buf, ncap);
            cap = ncap;
        }
        usize n = 0;
        StreamStatus st = dispatch_read(s, buf + len, 4096, &n);
        if (st == STREAM_ERR) {
            cando_free(buf);
            cando_vm_error(vm, "stream:readAll: %s",
                           s->last_err[0] ? s->last_err : "read failed");
            return -1;
        }
        len += n;
        if (st == STREAM_EOF) break;
        if (st == STREAM_AGAIN && n == 0) break;
    }
    atomic_fetch_add(&s->bytes_in, (u64)len);
    libutil_push_str(vm, (const char *)buf, (u32)len);
    cando_free(buf);
    return 1;
}

/* stream:write(data) -> bytesWritten */
static int stream_write_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:write");
    if (!s) return -1;
    if (!(s->caps & STREAM_CAP_WRITABLE)) {
        cando_vm_error(vm, "stream:write: not a writable stream");
        return -1;
    }
    if (atomic_load(&s->write_ended) || atomic_load(&s->closed)) {
        cando_vm_error(vm, "stream:write: write side is closed");
        return -1;
    }
    CandoString *data = libutil_arg_str_at(args, argc, 1);
    if (!data) {
        cando_vm_error(vm, "stream:write: expected string");
        return -1;
    }
    usize n = 0;
    StreamStatus st = dispatch_write(s, (const u8 *)data->data,
                                     data->length, &n);
    if (st == STREAM_ERR) {
        cando_vm_error(vm, "stream:write: %s",
                       s->last_err[0] ? s->last_err : "write failed");
        return -1;
    }
    atomic_fetch_add(&s->bytes_out, (u64)n);
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

/* stream:writeAll(data) -> self  (loops until all bytes consumed) */
static int stream_writeAll_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:writeAll");
    if (!s) return -1;
    if (!(s->caps & STREAM_CAP_WRITABLE)) {
        cando_vm_error(vm, "stream:writeAll: not a writable stream");
        return -1;
    }
    if (atomic_load(&s->write_ended) || atomic_load(&s->closed)) {
        cando_vm_error(vm, "stream:writeAll: write side is closed");
        return -1;
    }
    CandoString *data = libutil_arg_str_at(args, argc, 1);
    if (!data) {
        cando_vm_error(vm, "stream:writeAll: expected string");
        return -1;
    }
    usize off = 0;
    while (off < data->length) {
        usize n = 0;
        StreamStatus st = dispatch_write(s, (const u8 *)data->data + off,
                                         data->length - off, &n);
        if (st == STREAM_ERR) {
            cando_vm_error(vm, "stream:writeAll: %s",
                           s->last_err[0] ? s->last_err : "write failed");
            return -1;
        }
        off += n;
        if (st == STREAM_EOF) break;
        if (n == 0 && st != STREAM_AGAIN) break;
    }
    atomic_fetch_add(&s->bytes_out, (u64)off);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* stream:flush() -> self */
static int stream_flush_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:flush");
    if (!s) return -1;
    if (s->vt->flush) {
        StreamStatus st;
        if (s->vt->self_locked) {
            st = s->vt->flush(s->ctx);
        } else {
            CANDO_WITH_WRITE_LOCK(&s->lock) { st = s->vt->flush(s->ctx); }
        }
        if (st == STREAM_ERR) {
            cando_vm_error(vm, "stream:flush: %s",
                           s->last_err[0] ? s->last_err : "flush failed");
            return -1;
        }
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

/* stream:end() -> self  (half-close write side) */
static int stream_end_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = receiver_or_throw(vm, argc, args, "stream:end");
    if (!s) return -1;
    bool was = atomic_exchange(&s->write_ended, true);
    if (!was && s->vt->end) {
        StreamStatus st;
        if (s->vt->self_locked) {
            st = s->vt->end(s->ctx);
        } else {
            CANDO_WITH_WRITE_LOCK(&s->lock) { st = s->vt->end(s->ctx); }
        }
        if (st == STREAM_ERR) {
            cando_vm_error(vm, "stream:end: %s",
                           s->last_err[0] ? s->last_err : "end failed");
            return -1;
        }
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

/* stream:close() -> self  (full close, idempotent) */
static int stream_close_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    if (s) {
        /* Find the slot index so we can release through the pool API.  We
         * have the pointer; recover the index by pointer arithmetic. */
        int idx = (int)(s - g_stream_pool);
        stream_pool_release(idx);
    }
    cando_vm_push(vm, argc > 0 ? args[0] : cando_null());
    return 1;
}

/* stream:isClosed() -> bool */
static int stream_isClosed_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_bool(!s || atomic_load(&s->closed)));
    return 1;
}

/* stream:error() -> string ("" if no error) */
static int stream_error_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    if (!s || s->last_err[0] == '\0') {
        libutil_push_str(vm, "", 0);
        return 1;
    }
    libutil_push_cstr(vm, s->last_err);
    return 1;
}

/* stream:bytesIn() -> number */
static int stream_bytesIn_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_number(s ? (f64)atomic_load(&s->bytes_in) : 0.0));
    return 1;
}

/* stream:bytesOut() -> number */
static int stream_bytesOut_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_number(s ? (f64)atomic_load(&s->bytes_out) : 0.0));
    return 1;
}

/* stream:kind() -> string */
static int stream_kind_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *s = stream_resolve_receiver(vm,
                        argc > 0 ? args[0] : cando_null());
    const char *name = (s && s->vt && s->vt->kind_name) ? s->vt->kind_name : "";
    libutil_push_cstr(vm, name);
    return 1;
}

/* =========================================================================
 * Pipe driver
 * ===================================================================== */

int cando_stream_pipe(int src_idx, int dst_idx, usize chunk, u64 *copied)
{
    StreamSlot *src = stream_pool_get(src_idx);
    StreamSlot *dst = stream_pool_get(dst_idx);
    if (!src || !dst) return -1;
    if (!(src->caps & STREAM_CAP_READABLE)) return -1;
    if (!(dst->caps & STREAM_CAP_WRITABLE)) return -1;

    /* Clamp chunk size: too small is wasteful, too large pins too much
     * scratch RAM during long-running transfers. */
    if (chunk == 0)              chunk = 64 * 1024;
    if (chunk < 256)             chunk = 256;
    if (chunk > 4 * 1024 * 1024) chunk = 4 * 1024 * 1024;

    u8  *buf   = (u8 *)cando_alloc(chunk);
    u64  total = 0;
    int  rc    = 0;

    while (!atomic_load(&src->closed) && !atomic_load(&dst->closed)) {
        usize n = 0;
        StreamStatus rs = dispatch_read(src, buf, chunk, &n);
        if (rs == STREAM_ERR) { rc = -1; break; }
        if (n > 0) {
            atomic_fetch_add(&src->bytes_in, (u64)n);
            usize off = 0;
            bool  werr = false;
            while (off < n) {
                usize w = 0;
                StreamStatus ws = dispatch_write(dst, buf + off, n - off, &w);
                if (ws == STREAM_ERR) { werr = true; break; }
                off += w;
                /* For non-blocking sinks STREAM_AGAIN with no progress means
                 * "stop trying"; fall through to the outer loop. */
                if (w == 0 && ws == STREAM_AGAIN) break;
                if (w == 0 && ws == STREAM_EOF)   break;
            }
            atomic_fetch_add(&dst->bytes_out, (u64)off);
            total += off;
            if (werr) { rc = -1; break; }
        }
        if (rs == STREAM_EOF)                 break;
        if (rs == STREAM_AGAIN && n == 0)     break;
    }

    cando_free(buf);
    if (copied) *copied = total;
    return rc;
}

/* stream:pipeTo(dst [, opts]) -> bytesCopied
 *
 * Named `pipeTo` rather than `pipe` because `pipe` is a CanDo keyword (the
 * implicit loop variable in `~>` pipe expressions) and isn't permitted
 * after `:` in method-call syntax.
 */
static int stream_pipe_fn(CandoVM *vm, int argc, CandoValue *args)
{
    StreamSlot *src = receiver_or_throw(vm, argc, args, "stream:pipeTo");
    if (!src) return -1;
    if (argc < 2 || !cando_is_object(args[1])) {
        cando_vm_error(vm, "stream:pipeTo: destination stream required");
        return -1;
    }
    StreamSlot *dst = stream_resolve_receiver(vm, args[1]);
    if (!dst) {
        cando_vm_error(vm, "stream:pipeTo: invalid destination stream");
        return -1;
    }
    if (!(src->caps & STREAM_CAP_READABLE)) {
        cando_vm_error(vm, "stream:pipeTo: source is not readable");
        return -1;
    }
    if (!(dst->caps & STREAM_CAP_WRITABLE)) {
        cando_vm_error(vm, "stream:pipeTo: destination is not writable");
        return -1;
    }

    usize chunk = 64 * 1024;
    if (argc >= 3 && cando_is_object(args[2])) {
        CdoObject *opts = cando_bridge_resolve(vm, cando_as_handle(args[2]));
        int c = 0;
        if (opts && get_int_field(opts, "chunk", &c) && c > 0) {
            chunk = (usize)c;
        }
    }

    int src_idx = (int)(src - g_stream_pool);
    int dst_idx = (int)(dst - g_stream_pool);
    u64 copied  = 0;
    int rc = cando_stream_pipe(src_idx, dst_idx, chunk, &copied);
    if (rc < 0) {
        cando_vm_error(vm, "stream:pipeTo: %s",
                       src->last_err[0] ? src->last_err :
                       dst->last_err[0] ? dst->last_err : "pipe failed");
        return -1;
    }
    cando_vm_push(vm, cando_number((f64)copied));
    return 1;
}

/* =========================================================================
 * In-memory adapter (`stream.memory`)
 *
 * Append-only growing byte buffer with a read cursor.  Reads consume from
 * the cursor; writes append at the tail.  When the cursor catches up to
 * the tail and `write_ended` is set, reads return EOF.
 *
 * This is single-process scratch storage; the per-slot R/W lock taken by
 * the common methods makes it safe to use from a child VM.
 * ===================================================================== */

typedef struct MemCtx {
    u8       *buf;
    usize     cap;
    usize     len;       /* total bytes ever written                  */
    usize     read_pos;  /* read cursor; bytes 0..read_pos consumed   */
    bool      write_ended;
    StreamSlot *owner;   /* back-pointer for setting last_err         */
} MemCtx;

static void mem_compact_if_drained(MemCtx *m)
{
    /* If the reader has consumed everything, reset both cursors so the
     * buffer doesn't grow without bound during long-running pipes. */
    if (m->read_pos > 0 && m->read_pos == m->len) {
        m->read_pos = 0;
        m->len      = 0;
    }
}

static StreamStatus mem_read(void *vctx, u8 *out, usize cap, usize *n_out)
{
    MemCtx *m = (MemCtx *)vctx;
    usize avail = m->len - m->read_pos;
    if (avail == 0) {
        *n_out = 0;
        return m->write_ended ? STREAM_EOF : STREAM_AGAIN;
    }
    usize n = cap < avail ? cap : avail;
    memcpy(out, m->buf + m->read_pos, n);
    m->read_pos += n;
    *n_out = n;
    mem_compact_if_drained(m);
    return STREAM_OK;
}

static StreamStatus mem_write(void *vctx, const u8 *buf, usize len, usize *n_out)
{
    MemCtx *m = (MemCtx *)vctx;
    if (m->write_ended) {
        *n_out = 0;
        stream_set_error(m->owner, "memory stream: write after end");
        return STREAM_ERR;
    }
    /* Compact first so a steady-state pipe stays bounded by `cap`. */
    mem_compact_if_drained(m);
    if (m->len + len > m->cap) {
        usize ncap = m->cap ? m->cap : 4096;
        while (m->len + len > ncap) ncap *= 2;
        m->buf = (u8 *)cando_realloc(m->buf, ncap);
        m->cap = ncap;
    }
    memcpy(m->buf + m->len, buf, len);
    m->len += len;
    *n_out  = len;
    return STREAM_OK;
}

static StreamStatus mem_end(void *vctx)
{
    MemCtx *m = (MemCtx *)vctx;
    m->write_ended = true;
    return STREAM_OK;
}

static void mem_destroy(void *vctx)
{
    MemCtx *m = (MemCtx *)vctx;
    if (!m) return;
    cando_free(m->buf);
    cando_free(m);
}

static const StreamVTable g_mem_vt = {
    .read       = mem_read,
    .write      = mem_write,
    .flush      = NULL,
    .end        = mem_end,
    .destroy    = mem_destroy,
    .seek       = NULL,
    .tell       = NULL,
    .kind_name  = "memory",
};

/* =========================================================================
 * Channel adapter (`stream.channel`)
 *
 * Bounded thread channel: a circular byte buffer guarded by one mutex and
 * two condvars (`not_empty`, `not_full`).  The vtable is `self_locked` so
 * a blocked reader/writer does not hold the slot lock against its peer.
 *
 * Lifecycle:
 *   - read blocks while empty AND !write_ended; drains then returns EOF.
 *   - write blocks while full AND !closed; errors after `:end()`.
 *   - destroy wakes any waiter so close() / pool release returns promptly.
 *
 * The channel is by design the supported way to *transfer* bytes between
 * VM threads — it has no underlying transport.
 * ===================================================================== */

typedef struct ChanCtx {
    u8              *buf;
    usize            cap;
    usize            head;          /* read cursor                          */
    usize            tail;          /* write cursor                         */
    usize            len;           /* bytes available to read              */
    bool             write_ended;
    bool             destroyed;
    cando_mutex_t    mu;
    cando_cond_t     not_empty;
    cando_cond_t     not_full;
    StreamSlot      *owner;
} ChanCtx;

static StreamStatus chan_read(void *vctx, u8 *out, usize cap, usize *n_out)
{
    ChanCtx *c = (ChanCtx *)vctx;
    cando_os_mutex_lock(&c->mu);
    while (c->len == 0 && !c->write_ended && !c->destroyed) {
        cando_os_cond_wait(&c->not_empty, &c->mu);
    }
    if (c->len == 0) {
        cando_os_mutex_unlock(&c->mu);
        *n_out = 0;
        return c->destroyed ? STREAM_ERR : STREAM_EOF;
    }
    /* Copy out at most `cap` bytes, possibly wrapping around the ring. */
    usize n = cap < c->len ? cap : c->len;
    usize first = c->cap - c->head;
    if (first > n) first = n;
    memcpy(out, c->buf + c->head, first);
    if (n > first) {
        memcpy(out + first, c->buf, n - first);
    }
    c->head = (c->head + n) % c->cap;
    c->len -= n;
    cando_os_cond_broadcast(&c->not_full);
    cando_os_mutex_unlock(&c->mu);
    *n_out = n;
    return STREAM_OK;
}

static StreamStatus chan_write(void *vctx, const u8 *buf, usize len, usize *n_out)
{
    ChanCtx *c = (ChanCtx *)vctx;
    if (len == 0) { *n_out = 0; return STREAM_OK; }
    cando_os_mutex_lock(&c->mu);
    if (c->write_ended || c->destroyed) {
        cando_os_mutex_unlock(&c->mu);
        *n_out = 0;
        stream_set_error(c->owner, "channel: write after end");
        return STREAM_ERR;
    }
    /* Block until at least one byte of space is free.  We accept partial
     * writes to drain whatever space is available immediately, matching
     * the convention of the other adapters. */
    while (c->len == c->cap && !c->destroyed && !c->write_ended) {
        cando_os_cond_wait(&c->not_full, &c->mu);
    }
    if (c->destroyed || c->write_ended) {
        cando_os_mutex_unlock(&c->mu);
        *n_out = 0;
        stream_set_error(c->owner, "channel: closed during write");
        return STREAM_ERR;
    }
    usize free = c->cap - c->len;
    usize n    = len < free ? len : free;
    usize first = c->cap - c->tail;
    if (first > n) first = n;
    memcpy(c->buf + c->tail, buf, first);
    if (n > first) {
        memcpy(c->buf, buf + first, n - first);
    }
    c->tail = (c->tail + n) % c->cap;
    c->len += n;
    cando_os_cond_broadcast(&c->not_empty);
    cando_os_mutex_unlock(&c->mu);
    *n_out = n;
    return STREAM_OK;
}

static StreamStatus chan_end(void *vctx)
{
    ChanCtx *c = (ChanCtx *)vctx;
    cando_os_mutex_lock(&c->mu);
    c->write_ended = true;
    cando_os_cond_broadcast(&c->not_empty);
    cando_os_cond_broadcast(&c->not_full);
    cando_os_mutex_unlock(&c->mu);
    return STREAM_OK;
}

static void chan_destroy(void *vctx)
{
    ChanCtx *c = (ChanCtx *)vctx;
    if (!c) return;
    /* Wake any waiter, then tear down.  Caller holds the pool mutex so no
     * fresh read/write can enter once we're past the wakeup. */
    cando_os_mutex_lock(&c->mu);
    c->destroyed   = true;
    c->write_ended = true;
    cando_os_cond_broadcast(&c->not_empty);
    cando_os_cond_broadcast(&c->not_full);
    cando_os_mutex_unlock(&c->mu);
    cando_os_cond_destroy(&c->not_empty);
    cando_os_cond_destroy(&c->not_full);
    cando_os_mutex_destroy(&c->mu);
    cando_free(c->buf);
    cando_free(c);
}

static const StreamVTable g_chan_vt = {
    .read        = chan_read,
    .write       = chan_write,
    .flush       = NULL,
    .end         = chan_end,
    .destroy     = chan_destroy,
    .seek        = NULL,
    .tell        = NULL,
    .kind_name   = "channel",
    .self_locked = true,
};

/* =========================================================================
 * Transform adapter (`stream.transform(fn)`)
 *
 * Duplex stream that pipes every chunk through a user-supplied CanDo
 * function:  write(bytes) → fn(bytes) → output buffer ← read(bytes).
 *
 * The transform owns a child VM (cando_vm_init_child) so the function
 * can be invoked from non-script threads (e.g. inside a pipe driver) the
 * same way socket/http server callbacks already do.  The internal mutex
 * serialises `fn` calls — only one write may be in flight at a time —
 * which keeps the child VM single-consumer and matches the user's
 * mental model that a transform is sequential.
 *
 * Contract for `fn`:
 *   - takes one string argument (the input chunk; may be partial)
 *   - returns a string (the transformed bytes; may be empty to drop the
 *     chunk) — non-string returns are ignored
 *
 * Backpressure: writes block other writers via the mutex but never the
 * reader.  Readers block when the output buffer is empty until either a
 * new write fills it or `:end()` is called.
 * ===================================================================== */

typedef struct TransformCtx {
    CandoVM         child_vm;
    bool            vm_inited;
    CandoValue      fn;
    /* Output buffer: append-only growing byte buffer with a read cursor.
     * Compacts when fully drained, mirroring MemCtx. */
    u8             *out_buf;
    usize           out_cap;
    usize           out_len;        /* bytes available to read */
    bool            write_ended;
    cando_mutex_t   mu;
    cando_cond_t    not_empty;
    StreamSlot     *owner;
} TransformCtx;

static void transform_out_append(TransformCtx *t, const u8 *data, usize len)
{
    if (len == 0) return;
    if (t->out_len + len > t->out_cap) {
        usize ncap = t->out_cap ? t->out_cap : 4096;
        while (t->out_len + len > ncap) ncap *= 2;
        t->out_buf = (u8 *)cando_realloc(t->out_buf, ncap);
        t->out_cap = ncap;
    }
    memcpy(t->out_buf + t->out_len, data, len);
    t->out_len += len;
}

static StreamStatus transform_write(void *vctx, const u8 *buf, usize len,
                                    usize *n_out)
{
    TransformCtx *t = (TransformCtx *)vctx;
    if (len == 0) { *n_out = 0; return STREAM_OK; }

    cando_os_mutex_lock(&t->mu);
    if (t->write_ended) {
        cando_os_mutex_unlock(&t->mu);
        *n_out = 0;
        stream_set_error(t->owner, "transform: write after end");
        return STREAM_ERR;
    }

    /* Build the input string and call the user transform.  The mutex is
     * held for the duration so the child VM is exclusively ours; the
     * function body cannot race against another write on this stream. */
    CandoString *in_s = cando_string_new((const char *)buf, (u32)len);
    CandoValue   arg  = cando_string_value(in_s);

    int n_ret = cando_vm_call_value(&t->child_vm, t->fn, &arg, 1);
    cando_string_release(in_s);

    /* Surface uncaught errors from the user transform so they don't
     * vanish into the streaming pipeline.                             */
    if (t->child_vm.has_error) {
        cando_vm_log_uncaught(&t->child_vm, "stream transform callback");
    }

    /* Drain returned values; only the first string is appended to the
     * output buffer.  Anything else is silently dropped — matches the
     * "filter chunk" semantics where returning null/non-string skips. */
    if (n_ret > 0) {
        CandoValue r = cando_vm_pop(&t->child_vm);
        if (cando_is_string(r) && r.as.string && r.as.string->length > 0) {
            transform_out_append(t,
                                 (const u8 *)r.as.string->data,
                                 r.as.string->length);
            cando_os_cond_broadcast(&t->not_empty);
        }
        cando_value_release(r);
        for (int i = 1; i < n_ret; i++) {
            CandoValue extra = cando_vm_pop(&t->child_vm);
            cando_value_release(extra);
        }
    }

    cando_os_mutex_unlock(&t->mu);
    *n_out = len;
    return STREAM_OK;
}

static StreamStatus transform_read(void *vctx, u8 *out, usize cap, usize *n_out)
{
    TransformCtx *t = (TransformCtx *)vctx;
    cando_os_mutex_lock(&t->mu);
    while (t->out_len == 0 && !t->write_ended) {
        cando_os_cond_wait(&t->not_empty, &t->mu);
    }
    if (t->out_len == 0) {
        cando_os_mutex_unlock(&t->mu);
        *n_out = 0;
        return STREAM_EOF;
    }
    usize n = cap < t->out_len ? cap : t->out_len;
    memcpy(out, t->out_buf, n);
    if (n < t->out_len) {
        memmove(t->out_buf, t->out_buf + n, t->out_len - n);
    }
    t->out_len -= n;
    cando_os_mutex_unlock(&t->mu);
    *n_out = n;
    return STREAM_OK;
}

static StreamStatus transform_end(void *vctx)
{
    TransformCtx *t = (TransformCtx *)vctx;
    cando_os_mutex_lock(&t->mu);
    t->write_ended = true;
    cando_os_cond_broadcast(&t->not_empty);
    cando_os_mutex_unlock(&t->mu);
    return STREAM_OK;
}

static void transform_destroy(void *vctx)
{
    TransformCtx *t = (TransformCtx *)vctx;
    if (!t) return;
    cando_os_mutex_lock(&t->mu);
    t->write_ended = true;
    cando_os_cond_broadcast(&t->not_empty);
    cando_os_mutex_unlock(&t->mu);

    if (t->vm_inited) {
        cando_vm_destroy(&t->child_vm);
        t->vm_inited = false;
    }
    cando_value_release(t->fn);
    cando_free(t->out_buf);
    cando_os_cond_destroy(&t->not_empty);
    cando_os_mutex_destroy(&t->mu);
    cando_free(t);
}

static const StreamVTable g_transform_vt = {
    .read        = transform_read,
    .write       = transform_write,
    .flush       = NULL,
    .end         = transform_end,
    .destroy     = transform_destroy,
    .seek        = NULL,
    .tell        = NULL,
    .kind_name   = "transform",
    .self_locked = true,
};

/* stream.transform(fn) -> stream
 *
 * fn(chunk) → chunk: returns the transformed bytes for each input chunk.
 * Return null or a non-string to drop the chunk.  The transform is
 * applied eagerly on each :write so reads only see already-transformed
 * bytes.
 */
static int mod_transform_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm, "stream.transform: function required");
        return -1;
    }
    TransformCtx *t = (TransformCtx *)cando_alloc(sizeof(TransformCtx));
    memset(t, 0, sizeof(*t));
    cando_vm_init_child(&t->child_vm, vm);
    t->vm_inited = true;
    t->fn = cando_value_copy(args[0]);
    cando_os_mutex_init(&t->mu);
    cando_os_cond_init(&t->not_empty);

    int idx = stream_pool_alloc(&g_transform_vt, t, STREAM_CAP_DUPLEX);
    if (idx < 0) {
        cando_value_release(t->fn);
        cando_vm_destroy(&t->child_vm);
        cando_os_cond_destroy(&t->not_empty);
        cando_os_mutex_destroy(&t->mu);
        cando_free(t);
        cando_vm_error(vm, "stream.transform: too many active streams");
        return -1;
    }
    t->owner = stream_pool_get(idx);
    cando_vm_push(vm, stream_create_instance(vm, idx));
    return 1;
}

/* stream.channel(capacity?) -> stream */
static int mod_channel_fn(CandoVM *vm, int argc, CandoValue *args)
{
    usize cap = (usize)libutil_arg_num_at(args, argc, 0, 65536.0);
    if (cap < 64)              cap = 64;
    if (cap > 64 * 1024 * 1024) cap = 64 * 1024 * 1024;

    ChanCtx *c = (ChanCtx *)cando_alloc(sizeof(ChanCtx));
    memset(c, 0, sizeof(*c));
    c->buf = (u8 *)cando_alloc(cap);
    c->cap = cap;
    cando_os_mutex_init(&c->mu);
    cando_os_cond_init(&c->not_empty);
    cando_os_cond_init(&c->not_full);

    int idx = stream_pool_alloc(&g_chan_vt, c, STREAM_CAP_DUPLEX);
    if (idx < 0) {
        cando_os_cond_destroy(&c->not_empty);
        cando_os_cond_destroy(&c->not_full);
        cando_os_mutex_destroy(&c->mu);
        cando_free(c->buf);
        cando_free(c);
        cando_vm_error(vm, "stream.channel: too many active streams");
        return -1;
    }
    c->owner = stream_pool_get(idx);
    cando_vm_push(vm, stream_create_instance(vm, idx));
    return 1;
}

/* Public helper: build a pre-filled, write-ended memory stream.  See
 * stream.h for the contract. */
CandoValue cando_stream_from_bytes(CandoVM *vm, const void *data, usize len)
{
    usize cap = len > 0 ? len : 64;

    MemCtx *m = (MemCtx *)cando_alloc(sizeof(MemCtx));
    memset(m, 0, sizeof(*m));
    m->buf = (u8 *)cando_alloc(cap);
    m->cap = cap;
    if (len > 0) {
        memcpy(m->buf, data, len);
        m->len = len;
    }
    m->write_ended = true;          /* read-only view */

    int idx = stream_pool_alloc(&g_mem_vt, m, STREAM_CAP_READABLE);
    if (idx < 0) {
        cando_free(m->buf);
        cando_free(m);
        return cando_null();
    }
    m->owner = stream_pool_get(idx);
    return stream_create_instance(vm, idx);
}

/* stream.memory(initialBytes?) -> stream */
static int mod_memory_fn(CandoVM *vm, int argc, CandoValue *args)
{
    usize initial = (usize)libutil_arg_num_at(args, argc, 0, 4096.0);
    if (initial < 64) initial = 64;
    if (initial > 64 * 1024 * 1024) initial = 64 * 1024 * 1024;

    MemCtx *m = (MemCtx *)cando_alloc(sizeof(MemCtx));
    memset(m, 0, sizeof(*m));
    m->buf = (u8 *)cando_alloc(initial);
    m->cap = initial;

    int idx = stream_pool_alloc(&g_mem_vt, m, STREAM_CAP_DUPLEX);
    if (idx < 0) {
        cando_free(m->buf);
        cando_free(m);
        cando_vm_error(vm, "stream.memory: too many active streams");
        return -1;
    }
    /* Wire the back-pointer so mem_write can record errors on the slot. */
    m->owner = stream_pool_get(idx);
    cando_vm_push(vm, stream_create_instance(vm, idx));
    return 1;
}

/* =========================================================================
 * Registration
 * ===================================================================== */

static void define_meta(CandoVM *vm, CdoObject *tbl)
{
    if (!tbl) return;
    cando_lib_meta_define(vm, tbl, "read",      stream_read_fn);
    cando_lib_meta_define(vm, tbl, "readAll",   stream_readAll_fn);
    cando_lib_meta_define(vm, tbl, "write",     stream_write_fn);
    cando_lib_meta_define(vm, tbl, "writeAll",  stream_writeAll_fn);
    cando_lib_meta_define(vm, tbl, "flush",     stream_flush_fn);
    cando_lib_meta_define(vm, tbl, "end",       stream_end_fn);
    cando_lib_meta_define(vm, tbl, "close",     stream_close_fn);
    cando_lib_meta_define(vm, tbl, "isClosed",  stream_isClosed_fn);
    cando_lib_meta_define(vm, tbl, "error",     stream_error_fn);
    cando_lib_meta_define(vm, tbl, "bytesIn",   stream_bytesIn_fn);
    cando_lib_meta_define(vm, tbl, "bytesOut",  stream_bytesOut_fn);
    cando_lib_meta_define(vm, tbl, "kind",      stream_kind_fn);
    cando_lib_meta_define(vm, tbl, "pipeTo",    stream_pipe_fn);
}

void cando_lib_stream_register(CandoVM *vm)
{
    ensure_pool_inited();
    cando_lib_meta_register(vm);

    /* Module globals. */
    CandoValue mod_val = cando_bridge_new_object(vm);
    CdoObject *mod_obj = cando_bridge_resolve(vm, cando_as_handle(mod_val));
    libutil_set_method(vm, mod_obj, "memory",    mod_memory_fn);
    libutil_set_method(vm, mod_obj, "channel",   mod_channel_fn);
    libutil_set_method(vm, mod_obj, "transform", mod_transform_fn);
    cando_vm_set_global(vm, "stream", mod_val, true);

    /* Meta table for every stream instance. */
    CdoObject *meta = cando_lib_meta_table(vm, "stream");
    define_meta(vm, meta);
}
