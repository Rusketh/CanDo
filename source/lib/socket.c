/*
 * lib/socket.c -- Raw TCP socket library (the `socket` global) for CanDo.
 *
 * Layered structure:
 *   1. Pool of SocketSlot, with mutex-guarded alloc/free.
 *   2. Receiver helpers (look up SocketSlot from CanDo `__socket_id` field).
 *   3. Connection-level methods (registered on `_meta.tcp_socket`).
 *   4. Listener-level methods (registered on `_meta.tcp_server`).
 *   5. Module-level functions: socket.tcp / .connect / .createServer / .resolve.
 *   6. Registration entry point.
 *
 * The pool, the metatable population, and the accept-loop machinery are
 * exported (via socket.h) so `secure_socket.c` can layer TLS on the same
 * primitives without copy/paste.
 *
 * Must compile with gcc -std=c11.
 */

#include "socket.h"
#include "sockutil.h"
#include "libutil.h"
#include "meta.h"
#include "stream.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"
#include "../core/thread_platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* =========================================================================
 * Pool
 * ===================================================================== */

static SocketSlot      g_socket_pool[SOCKET_MAX_INSTANCES];
static cando_mutex_t   g_socket_pool_mutex;
static _Atomic(int)    g_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_pool_inited, &expected, 1)) {
        cando_os_mutex_init(&g_socket_pool_mutex);
        for (int i = 0; i < SOCKET_MAX_INSTANCES; i++) {
            memset(&g_socket_pool[i], 0, sizeof(SocketSlot));
            g_socket_pool[i].fd = SOCKUTIL_INVALID_SOCKET;
        }
    }
}

int socket_pool_alloc(SocketKind kind)
{
    ensure_pool_inited();
    cando_os_mutex_lock(&g_socket_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SOCKET_MAX_INSTANCES; i++) {
        if (!g_socket_pool[i].in_use) {
            memset(&g_socket_pool[i], 0, sizeof(SocketSlot));
            g_socket_pool[i].in_use     = true;
            g_socket_pool[i].kind       = kind;
            g_socket_pool[i].fd         = SOCKUTIL_INVALID_SOCKET;
            g_socket_pool[i].callback_fn = cando_null();
            atomic_store(&g_socket_pool[i].running, false);
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_socket_pool_mutex);
    return idx;
}

SocketSlot *socket_pool_get(int idx)
{
    if (idx < 0 || idx >= SOCKET_MAX_INSTANCES) return NULL;
    if (!g_socket_pool[idx].in_use) return NULL;
    return &g_socket_pool[idx];
}

void socket_pool_release(int idx)
{
    if (idx < 0 || idx >= SOCKET_MAX_INSTANCES) return;
    cando_os_mutex_lock(&g_socket_pool_mutex);
    SocketSlot *s = &g_socket_pool[idx];
    if (!s->in_use) {
        cando_os_mutex_unlock(&g_socket_pool_mutex);
        return;
    }
    /* Tear down TLS first so SSL_shutdown's notify gets through before the
     * underlying fd is closed. */
    if (s->ssl) {
        sockutil_tls_free(s->ssl);
        s->ssl = NULL;
    }
    if (s->ssl_ctx && s->owns_ssl_ctx) {
        SSL_CTX_free(s->ssl_ctx);
    }
    s->ssl_ctx      = NULL;
    s->owns_ssl_ctx = false;

    if (s->fd != SOCKUTIL_INVALID_SOCKET) {
        sockutil_close(s->fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
    }
    if (s->sni_host) {
        free(s->sni_host);
        s->sni_host = NULL;
    }
    if (!cando_is_null(s->callback_fn)) {
        cando_value_release(s->callback_fn);
        s->callback_fn = cando_null();
    }
    s->in_use    = false;
    s->kind      = SOCK_KIND_UNUSED;
    s->connected = false;
    cando_os_mutex_unlock(&g_socket_pool_mutex);
}

/* =========================================================================
 * Field helpers (mirror the small set used by http.c)
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

static void set_str_field(CdoObject *obj, const char *name,
                          const char *data, u32 len)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoString *val = cdo_string_new(data, len);
    CdoValue   v   = cdo_string_value(val);
    cdo_object_rawset(obj, key, v, FIELD_NONE);
    cdo_value_release(v);
    cdo_string_release(key);
}

/* =========================================================================
 * Receiver resolution
 * ===================================================================== */

SocketSlot *socket_resolve_receiver(CandoVM *vm, CandoValue receiver)
{
    if (!cando_is_object(receiver)) return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(receiver));
    if (!obj) return NULL;
    int idx = -1;
    if (!get_int_field(obj, "__socket_id", &idx)) return NULL;
    return socket_pool_get(idx);
}

CandoValue socket_create_instance(CandoVM *vm, int slot_idx,
                                  const char *meta_name)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));
    set_num_field(obj, "__socket_id", (f64)slot_idx);
    cando_lib_meta_attach(vm, obj, meta_name);
    return val;
}

/* =========================================================================
 * Argument helpers
 * ===================================================================== */

/* Map an "inet"/"inet6"/"any" string (or NULL) to AF_INET/AF_INET6/AF_UNSPEC. */
static int parse_family(const char *s)
{
    if (!s || !*s) return AF_UNSPEC;
    if (strcmp(s, "inet")  == 0) return AF_INET;
    if (strcmp(s, "inet6") == 0) return AF_INET6;
    return AF_UNSPEC;
}

static const char *family_name(int af)
{
    if (af == AF_INET)  return "inet";
    if (af == AF_INET6) return "inet6";
    return "unspec";
}

/*
 * Read host/port/timeout/family/backlog from an options object (when
 * present) into out parameters.  Each out pointer may be NULL to skip.
 */
static void read_connect_opts(CandoVM *vm, CandoValue opts_val,
                              int *timeout_ms, int *family)
{
    if (!cando_is_object(opts_val)) return;
    CdoObject *opts = cando_bridge_resolve(vm, cando_as_handle(opts_val));
    if (!opts) return;

    if (timeout_ms) {
        int t = 0;
        if (get_int_field(opts, "timeout", &t) && t > 0) *timeout_ms = t;
    }
    if (family) {
        CdoString *key = cdo_string_intern("family", 6);
        CdoValue   v   = cdo_null();
        bool ok = cdo_object_rawget(opts, key, &v);
        cdo_string_release(key);
        if (ok && v.tag == CDO_STRING && v.as.string) {
            *family = parse_family(v.as.string->data);
        }
    }
}

/* =========================================================================
 * Connection-level methods (registered on _meta.tcp_socket).
 *
 * The "self" receiver is at args[0]; method-specific arguments start at
 * args[1].  Methods that mutate state return the receiver to support
 * chaining; data-returning methods return their value.
 *
 * I/O failures throw via cando_vm_error per the documented contract.  Clean
 * EOF on recv returns "" so that "while (data := recv) != ''" loops work.
 * ===================================================================== */

/*
 * conn_check -- common prologue for methods that require a connected socket.
 * Returns the slot or NULL after raising an error.
 */
static SocketSlot *conn_check(CandoVM *vm, CandoValue receiver,
                              const char *method, bool require_connected)
{
    SocketSlot *s = socket_resolve_receiver(vm, receiver);
    if (!s) {
        cando_vm_error(vm, "%s: invalid socket receiver", method);
        return NULL;
    }
    if (s->kind != SOCK_KIND_TCP && s->kind != SOCK_KIND_TLS) {
        cando_vm_error(vm, "%s: not a connection socket", method);
        return NULL;
    }
    if (require_connected) {
        if (!s->connected || s->fd == SOCKUTIL_INVALID_SOCKET) {
            cando_vm_error(vm, "%s: socket is not open", method);
            return NULL;
        }
    }
    return s;
}

/* tcp_socket:connect(host, port [, timeout_ms]) — synchronous connect. */
static int tcp_connect_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || (s->kind != SOCK_KIND_TCP && s->kind != SOCK_KIND_TLS)) {
        cando_vm_error(vm, "socket:connect: invalid receiver");
        return -1;
    }
    if (s->connected) {
        cando_vm_error(vm, "socket:connect: already connected");
        return -1;
    }
    const char *host = libutil_arg_cstr_at(args, argc, 1);
    if (!host) {
        cando_vm_error(vm, "socket:connect: host (string) required");
        return -1;
    }
    int port = (int)libutil_arg_num_at(args, argc, 2, -1);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "socket:connect: invalid port");
        return -1;
    }
    int timeout = (int)libutil_arg_num_at(args, argc, 3, 0);

    char errbuf[160] = {0};
    sockutil_socket_t fd = sockutil_tcp_connect(host, port, AF_UNSPEC,
                                                timeout, errbuf, sizeof(errbuf));
    if (fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_error(vm, "socket:connect: %s",
                       errbuf[0] ? errbuf : "connect failed");
        return -1;
    }
    s->fd          = fd;
    s->connected   = true;
    s->timeout_ms  = timeout;
    cando_vm_push(vm, args[0]);  /* return receiver for chaining */
    return 1;
}

/* tcp_socket:send(data) -> bytesSent */
static int tcp_send_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:send", true);
    if (!s) return -1;
    CandoString *data = libutil_arg_str_at(args, argc, 1);
    if (!data) {
        cando_vm_error(vm, "socket:send: expected string");
        return -1;
    }
    int n = s->ssl ? sockutil_tls_send(s->ssl, data->data, (int)data->length)
                   : sockutil_send_raw(s->fd, data->data, (int)data->length);
    if (n < 0) {
        cando_vm_error(vm, "socket:send: write failed");
        return -1;
    }
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

/* tcp_socket:sendAll(data) -> self  (loops until all bytes are written). */
static int tcp_sendAll_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:sendAll", true);
    if (!s) return -1;
    CandoString *data = libutil_arg_str_at(args, argc, 1);
    if (!data) {
        cando_vm_error(vm, "socket:sendAll: expected string");
        return -1;
    }
    if (!sockutil_send_all(s->fd, s->ssl, data->data, data->length)) {
        cando_vm_error(vm, "socket:sendAll: write failed");
        return -1;
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

/* tcp_socket:recv(maxLen [, timeout_ms]) -> string ("" on clean EOF). */
static int tcp_recv_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:recv", true);
    if (!s) return -1;
    int maxlen = (int)libutil_arg_num_at(args, argc, 1, 4096);
    if (maxlen <= 0) {
        cando_vm_error(vm, "socket:recv: maxLen must be positive");
        return -1;
    }
    /* Hard cap a single recv at 16 MB to bound transient allocations.  Larger
     * payloads can be assembled across multiple recv() calls or via recvAll. */
    if (maxlen > 16 * 1024 * 1024) maxlen = 16 * 1024 * 1024;

    if (argc >= 3 && cando_is_number(args[2])) {
        sockutil_set_timeout(s->fd, (int)cando_as_number(args[2]));
    }

    char *buf = (char *)malloc((usize)maxlen);
    if (!buf) {
        cando_vm_error(vm, "socket:recv: out of memory");
        return -1;
    }
    int n = s->ssl ? sockutil_tls_recv(s->ssl, buf, maxlen)
                   : sockutil_recv_raw(s->fd, buf, maxlen);
    if (n < 0) {
        free(buf);
        cando_vm_error(vm, "socket:recv: read failed");
        return -1;
    }
    /* n == 0 is a clean EOF — surface as empty string, no error. */
    libutil_push_str(vm, buf, (u32)n);
    free(buf);
    return 1;
}

/* tcp_socket:recvAll() -> string  (read until EOF or peer close). */
static int tcp_recvAll_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:recvAll", true);
    if (!s) return -1;
    /* Grow a scratch buffer; double on demand. */
    usize cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        cando_vm_error(vm, "socket:recvAll: out of memory");
        return -1;
    }
    while (true) {
        if (len + 4096 > cap) {
            usize ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                cando_vm_error(vm, "socket:recvAll: out of memory");
                return -1;
            }
            buf = nb; cap = ncap;
        }
        int n = s->ssl ? sockutil_tls_recv(s->ssl, buf + len, 4096)
                       : sockutil_recv_raw(s->fd, buf + len, 4096);
        if (n < 0) {
            free(buf);
            cando_vm_error(vm, "socket:recvAll: read failed");
            return -1;
        }
        if (n == 0) break;          /* clean EOF */
        len += (usize)n;
    }
    libutil_push_str(vm, buf, (u32)len);
    free(buf);
    return 1;
}

/*
 * tcp_socket:recvLine([maxLen]) -> string
 *
 * Reads up to and including the next '\n'.  A trailing "\r\n" or "\n" is
 * stripped so callers don't have to.  Returns "" on clean EOF before any
 * data arrives.  `maxLen` (default 65536) caps the line length to bound
 * memory; oversized lines throw.
 *
 * Implemented byte-at-a-time which is fine for line-oriented protocols on
 * loopback.  For high-throughput parsing users can do a buffered recv loop
 * themselves.
 */
static int tcp_recvLine_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:recvLine", true);
    if (!s) return -1;
    int maxlen = (int)libutil_arg_num_at(args, argc, 1, 65536);
    if (maxlen <= 0) maxlen = 65536;

    char *buf = (char *)malloc((usize)maxlen);
    if (!buf) {
        cando_vm_error(vm, "socket:recvLine: out of memory");
        return -1;
    }
    int len = 0;
    while (len < maxlen) {
        char ch;
        int n = s->ssl ? sockutil_tls_recv(s->ssl, &ch, 1)
                       : sockutil_recv_raw(s->fd, &ch, 1);
        if (n < 0) {
            free(buf);
            cando_vm_error(vm, "socket:recvLine: read failed");
            return -1;
        }
        if (n == 0) break;          /* EOF */
        if (ch == '\n') {
            if (len > 0 && buf[len - 1] == '\r') len--;
            break;
        }
        buf[len++] = ch;
    }
    if (len == maxlen) {
        free(buf);
        cando_vm_error(vm, "socket:recvLine: line exceeded %d bytes", maxlen);
        return -1;
    }
    libutil_push_str(vm, buf, (u32)len);
    free(buf);
    return 1;
}

/* tcp_socket:close() — idempotent. */
static int tcp_close_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    /* Tear down transport but keep the slot allocated until the callback
     * thread (or the script that created the client socket) explicitly
     * drops the receiver.  This way isOpen() reports false and any further
     * I/O fails cleanly. */
    if (s->ssl) {
        sockutil_tls_free(s->ssl);
        s->ssl = NULL;
    }
    if (s->ssl_ctx && s->owns_ssl_ctx) {
        SSL_CTX_free(s->ssl_ctx);
        s->ssl_ctx      = NULL;
        s->owns_ssl_ctx = false;
    }
    if (s->fd != SOCKUTIL_INVALID_SOCKET) {
        sockutil_close(s->fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
    }
    s->connected = false;
    cando_vm_push(vm, args[0]);
    return 1;
}

/* tcp_socket:isOpen() -> bool */
static int tcp_isOpen_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_bool(s && s->connected &&
                                 s->fd != SOCKUTIL_INVALID_SOCKET));
    return 1;
}

/* tcp_socket:setTimeout(ms) -> self */
static int tcp_setTimeout_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:setTimeout", false);
    if (!s) return -1;
    int ms = (int)libutil_arg_num_at(args, argc, 1, 0);
    s->timeout_ms = ms;
    if (s->fd != SOCKUTIL_INVALID_SOCKET) sockutil_set_timeout(s->fd, ms);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* tcp_socket:setBlocking(bool) -> self */
static int tcp_setBlocking_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:setBlocking", false);
    if (!s) return -1;
    bool blocking = (argc < 2) ? true
                  : (cando_is_bool(args[1]) ? cando_as_bool(args[1]) : true);
    if (s->fd != SOCKUTIL_INVALID_SOCKET)
        sockutil_set_blocking(s->fd, blocking);
    cando_vm_push(vm, args[0]);
    return 1;
}

/*
 * tcp_socket:setOption(name, value) -> self
 *
 * Recognised option names (string keys are lowercase to match the rest of
 * CanDo's networking surface):
 *   "tcp_nodelay"   bool
 *   "so_keepalive"  bool
 *   "so_reuseaddr"  bool
 *   "so_rcvbuf"     number (bytes)
 *   "so_sndbuf"     number (bytes)
 */
static int tcp_setOption_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:setOption", false);
    if (!s) return -1;
    const char *name = libutil_arg_cstr_at(args, argc, 1);
    if (!name) {
        cando_vm_error(vm, "socket:setOption: option name (string) required");
        return -1;
    }
    if (s->fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_push(vm, args[0]);
        return 1;
    }
    bool ok = false;
    if (strcmp(name, "tcp_nodelay") == 0) {
        bool v = (argc >= 3 && cando_is_bool(args[2])) ? cando_as_bool(args[2]) : true;
        ok = sockutil_set_nodelay(s->fd, v);
    } else if (strcmp(name, "so_keepalive") == 0) {
        bool v = (argc >= 3 && cando_is_bool(args[2])) ? cando_as_bool(args[2]) : true;
        ok = sockutil_set_keepalive(s->fd, v);
    } else if (strcmp(name, "so_reuseaddr") == 0) {
        bool v = (argc >= 3 && cando_is_bool(args[2])) ? cando_as_bool(args[2]) : true;
        ok = sockutil_set_reuseaddr(s->fd, v);
    } else if (strcmp(name, "so_rcvbuf") == 0) {
        int v = (int)libutil_arg_num_at(args, argc, 2, 0);
        ok = sockutil_set_recvbuf(s->fd, v);
    } else if (strcmp(name, "so_sndbuf") == 0) {
        int v = (int)libutil_arg_num_at(args, argc, 2, 0);
        ok = sockutil_set_sendbuf(s->fd, v);
    } else {
        cando_vm_error(vm, "socket:setOption: unknown option '%s'", name);
        return -1;
    }
    if (!ok) {
        cando_vm_error(vm, "socket:setOption: setsockopt failed for '%s'", name);
        return -1;
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

/* tcp_socket:fd() -> number  (escape hatch for advanced users). */
static int tcp_fd_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_number(s ? (f64)s->fd : -1.0));
    return 1;
}

/*
 * Build a CanDo `{ host, port, family }` object from a sockaddr_storage.
 * Pushed onto the stack and returned, or null on failure.
 */
static int push_addr_object(CandoVM *vm, const struct sockaddr_storage *sa,
                            socklen_t len)
{
    char host[NI_MAXHOST] = {0};
    int  port = 0, family = 0;
    if (!sockutil_addr_to_string(sa, len, host, sizeof(host), &port, &family)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    set_str_field(obj, "host", host, (u32)strlen(host));
    set_num_field(obj, "port", (f64)port);
    set_str_field(obj, "family", family_name(family),
                  (u32)strlen(family_name(family)));
    cando_vm_push(vm, obj_val);
    return 1;
}

/* tcp_socket:localAddress() -> { host, port, family } | null */
static int tcp_localAddress_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || s->fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    struct sockaddr_storage sa;
    socklen_t len = 0;
    if (!sockutil_get_local_addr(s->fd, &sa, &len)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    return push_addr_object(vm, &sa, len);
}

/* tcp_socket:remoteAddress() -> { host, port, family } | null */
static int tcp_remoteAddress_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || s->fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    struct sockaddr_storage sa;
    socklen_t len = 0;
    if (!sockutil_get_peer_addr(s->fd, &sa, &len)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    return push_addr_object(vm, &sa, len);
}

/* =========================================================================
 * Stream adapter (registered as `tcp_socket:stream()`)
 *
 * Wraps an existing SocketSlot fd (and SSL*, for TLS) behind the unified
 * stream vtable.  The stream does NOT own the underlying socket — the
 * tcp_socket script object retains ownership.  When the user closes the
 * socket independently the next read/write on the stream surfaces a
 * STREAM_ERR with "socket closed".
 *
 * `self_locked = true` because sockets are full-duplex and a blocked
 * recv() must not hold the slot lock against a concurrent send().
 * ===================================================================== */

typedef struct SockStreamCtx {
    int  socket_idx;        /* index into the socket pool                 */
} SockStreamCtx;

static StreamStatus sock_stream_read(void *vctx, u8 *out, usize cap, usize *n_out)
{
    SockStreamCtx *sc = (SockStreamCtx *)vctx;
    SocketSlot    *s  = socket_pool_get(sc->socket_idx);
    if (!s || s->fd == SOCKUTIL_INVALID_SOCKET || !s->connected) {
        *n_out = 0;
        return STREAM_EOF;
    }
    int ilen = (int)(cap > (usize)INT_MAX ? INT_MAX : cap);
    int n = s->ssl ? sockutil_tls_recv(s->ssl, out, ilen)
                   : sockutil_recv_raw(s->fd,  out, ilen);
    if (n < 0)  { *n_out = 0; return STREAM_ERR; }
    if (n == 0) { *n_out = 0; return STREAM_EOF; }
    *n_out = (usize)n;
    return STREAM_OK;
}

static StreamStatus sock_stream_write(void *vctx, const u8 *buf, usize len,
                                      usize *n_out)
{
    SockStreamCtx *sc = (SockStreamCtx *)vctx;
    SocketSlot    *s  = socket_pool_get(sc->socket_idx);
    if (!s || s->fd == SOCKUTIL_INVALID_SOCKET || !s->connected) {
        *n_out = 0;
        return STREAM_ERR;
    }
    /* sockutil_send_all loops until done; that gives the caller the same
     * "writeAll succeeds in one hop" semantics they get from
     * tcp_socket:sendAll, which is what the stream contract expects. */
    if (!sockutil_send_all(s->fd, s->ssl, (const char *)buf, len)) {
        *n_out = 0;
        return STREAM_ERR;
    }
    *n_out = len;
    return STREAM_OK;
}

static void sock_stream_destroy(void *vctx)
{
    /* We do NOT close the underlying socket — the tcp_socket script object
     * still owns it.  Just free our wrapper. */
    cando_free(vctx);
}

static const StreamVTable g_sock_stream_vt = {
    .read        = sock_stream_read,
    .write       = sock_stream_write,
    .flush       = NULL,
    .end         = NULL,
    .destroy     = sock_stream_destroy,
    .seek        = NULL,
    .tell        = NULL,
    .kind_name   = "tcp",
    .self_locked = true,
};

static const StreamVTable g_tls_stream_vt = {
    .read        = sock_stream_read,
    .write       = sock_stream_write,
    .flush       = NULL,
    .end         = NULL,
    .destroy     = sock_stream_destroy,
    .seek        = NULL,
    .tell        = NULL,
    .kind_name   = "tls",
    .self_locked = true,
};

/* tcp_socket:stream() -> stream */
static int tcp_stream_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = conn_check(vm, argc > 0 ? args[0] : cando_null(),
                               "socket:stream", true);
    if (!s) return -1;
    int sock_idx = (int)(s - g_socket_pool);

    SockStreamCtx *ctx = (SockStreamCtx *)cando_alloc(sizeof(SockStreamCtx));
    ctx->socket_idx = sock_idx;

    const StreamVTable *vt = s->ssl ? &g_tls_stream_vt : &g_sock_stream_vt;
    int idx = stream_pool_alloc(vt, ctx, STREAM_CAP_DUPLEX);
    if (idx < 0) {
        cando_free(ctx);
        cando_vm_error(vm, "socket:stream: too many active streams");
        return -1;
    }
    cando_vm_push(vm, stream_create_instance(vm, idx));
    return 1;
}

void socket_meta_define_common(CandoVM *vm, CdoObject *tbl)
{
    if (!tbl) return;
    cando_lib_meta_define(vm, tbl, "connect",       tcp_connect_fn);
    cando_lib_meta_define(vm, tbl, "send",          tcp_send_fn);
    cando_lib_meta_define(vm, tbl, "sendAll",       tcp_sendAll_fn);
    cando_lib_meta_define(vm, tbl, "recv",          tcp_recv_fn);
    cando_lib_meta_define(vm, tbl, "recvAll",       tcp_recvAll_fn);
    cando_lib_meta_define(vm, tbl, "recvLine",      tcp_recvLine_fn);
    cando_lib_meta_define(vm, tbl, "close",         tcp_close_fn);
    cando_lib_meta_define(vm, tbl, "isOpen",        tcp_isOpen_fn);
    cando_lib_meta_define(vm, tbl, "setTimeout",    tcp_setTimeout_fn);
    cando_lib_meta_define(vm, tbl, "setBlocking",   tcp_setBlocking_fn);
    cando_lib_meta_define(vm, tbl, "setOption",     tcp_setOption_fn);
    cando_lib_meta_define(vm, tbl, "fd",            tcp_fd_fn);
    cando_lib_meta_define(vm, tbl, "localAddress",  tcp_localAddress_fn);
    cando_lib_meta_define(vm, tbl, "remoteAddress", tcp_remoteAddress_fn);
    cando_lib_meta_define(vm, tbl, "stream",        tcp_stream_fn);
}


/* =========================================================================
 * Per-connection child VM
 *
 * `socket_default_conn_handler` is what the accept loop calls for each
 * incoming connection on a plain TCP listener.  The flow mirrors http.c:
 *   - allocate a connection slot,
 *   - spin up a child VM sharing globals/handles with the parent,
 *   - construct the conn object and attach `_meta.tcp_socket`,
 *   - invoke the user callback synchronously,
 *   - on return, close the connection and release the slot.
 *
 * `socket_run_accept_loop` is the body of the listener's worker thread.  It
 * is parameterised on the per-connection handler so secure_socket.c can
 * provide a TLS-aware variant without forking the accept loop itself.
 * ===================================================================== */

void socket_default_conn_handler(int listener_idx, sockutil_socket_t cfd)
{
    SocketSlot *listener = socket_pool_get(listener_idx);
    if (!listener) {
        sockutil_close(cfd);
        return;
    }

    int conn_idx = socket_pool_alloc(SOCK_KIND_TCP);
    if (conn_idx < 0) {
        sockutil_close(cfd);
        return;
    }
    SocketSlot *conn = socket_pool_get(conn_idx);
    conn->fd        = cfd;
    conn->connected = true;

    CandoVM child;
    cando_vm_init_child(&child, listener->parent_vm);

    CandoValue conn_val = socket_create_instance(&child, conn_idx, "tcp_socket");

    CandoValue cb_args[1] = { conn_val };
    cando_vm_call_value(&child, listener->callback_fn, cb_args, 1);
    if (child.has_error) {
        cando_vm_log_uncaught(&child, "socket listener callback");
    }

    socket_pool_release(conn_idx);
    cando_vm_destroy(&child);
}

typedef struct ConnArg {
    int                 listener_idx;
    sockutil_socket_t   client_fd;
    SocketConnHandler   handler;
} ConnArg;

static CANDO_THREAD_RETURN conn_thread_fn(void *arg_p)
{
    ConnArg *arg = (ConnArg *)arg_p;
    int                 lidx    = arg->listener_idx;
    sockutil_socket_t   cfd     = arg->client_fd;
    SocketConnHandler   handler = arg->handler;
    free(arg);

    if (handler) handler(lidx, cfd);
    else         sockutil_close(cfd);
    return CANDO_THREAD_RETURN_VAL;
}

typedef struct AcceptArg {
    int               listener_idx;
    SocketConnHandler handler;
} AcceptArg;

static CANDO_THREAD_RETURN accept_thread_fn(void *arg_p)
{
    AcceptArg *arg = (AcceptArg *)arg_p;
    int               lidx    = arg->listener_idx;
    SocketConnHandler handler = arg->handler;
    free(arg);

    SocketSlot *listener = socket_pool_get(lidx);
    if (!listener) return CANDO_THREAD_RETURN_VAL;

    while (atomic_load(&listener->running) &&
           !cando_vm_quit_requested(listener->parent_vm)) {
        /* Short timeout so the loop re-checks `running` periodically. */
        sockutil_socket_t cfd = sockutil_tcp_accept(listener->fd, 500,
                                                    NULL, NULL, NULL, 0);
        if (cfd == SOCKUTIL_INVALID_SOCKET) continue;

        ConnArg *carg = (ConnArg *)malloc(sizeof(ConnArg));
        if (!carg) {
            sockutil_close(cfd);
            continue;
        }
        carg->listener_idx = lidx;
        carg->client_fd    = cfd;
        carg->handler      = handler;

        cando_thread_t t;
        if (!cando_os_thread_create(&t, conn_thread_fn, carg)) {
            sockutil_close(cfd);
            free(carg);
            continue;
        }
        cando_os_thread_detach(t);
    }
    /* Release the lifeline now that the listener has stopped.  The
     * server:close() join naturally synchronises with this -- after
     * join returns, count has dropped, so cando_vm_wait_all_lifelines
     * can wake. */
    if (listener->has_lifeline) {
        listener->has_lifeline = false;
        cando_vm_lifeline_release(listener->parent_vm);
    }
    return CANDO_THREAD_RETURN_VAL;
}

void socket_run_accept_loop(int listener_idx, SocketConnHandler handler)
{
    AcceptArg fake = { listener_idx, handler };
    AcceptArg *heap = (AcceptArg *)malloc(sizeof(AcceptArg));
    if (!heap) return;
    *heap = fake;
    accept_thread_fn(heap);
}

/* =========================================================================
 * Listener-level methods (registered on _meta.tcp_server).
 * ===================================================================== */

static SocketSlot *listener_check(CandoVM *vm, CandoValue receiver,
                                  const char *method)
{
    SocketSlot *s = socket_resolve_receiver(vm, receiver);
    if (!s) {
        cando_vm_error(vm, "%s: invalid server receiver", method);
        return NULL;
    }
    if (s->kind != SOCK_KIND_TCP_LISTENER && s->kind != SOCK_KIND_TLS_LISTENER) {
        cando_vm_error(vm, "%s: not a listener", method);
        return NULL;
    }
    return s;
}

/*
 * tcp_server:listen(port [, host [, backlog]]) -> self
 *
 * Mirrors http server's :listen exactly.  Returns immediately; the actual
 * accept loop runs on a background thread.  An optional callback may be
 * passed as the last positional argument to override the one given to
 * createServer.
 */
static int tcp_listen_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = listener_check(vm, argc > 0 ? args[0] : cando_null(),
                                   "server:listen");
    if (!s) return -1;
    int port = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "server:listen: invalid port");
        return -1;
    }
    const char *host = libutil_arg_cstr_at(args, argc, 2);
    int backlog = (int)libutil_arg_num_at(args, argc, 3, 64);

    /* Allow `:listen(port, callback)` as an alternate spelling. */
    if (argc >= 3 && cando_is_object(args[2]) && !cando_is_null(args[2])) {
        if (!cando_is_null(s->callback_fn)) cando_value_release(s->callback_fn);
        s->callback_fn = cando_value_copy(args[2]);
        host = NULL;
    }
    if (argc >= 4 && cando_is_object(args[3]) && !cando_is_null(args[3])) {
        if (!cando_is_null(s->callback_fn)) cando_value_release(s->callback_fn);
        s->callback_fn = cando_value_copy(args[3]);
    }

    if (cando_is_null(s->callback_fn)) {
        cando_vm_error(vm, "server:listen: no connection callback was set");
        return -1;
    }

    char errbuf[160] = {0};
    sockutil_socket_t fd = sockutil_tcp_listen(host, port, AF_UNSPEC,
                                               backlog, errbuf, sizeof(errbuf));
    if (fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_error(vm, "server:listen: %s",
                       errbuf[0] ? errbuf : "bind/listen failed");
        return -1;
    }
    s->fd = fd;
    atomic_store(&s->running, true);

    /* The accept worker is parameterised by listener kind: plain TCP
     * listeners use the default handler; secure_socket.c installs its own
     * via socket_run_accept_loop / socket_create_tls_listener. */
    SocketConnHandler handler = socket_default_conn_handler;
    if (s->kind == SOCK_KIND_TLS_LISTENER) {
        /* For TLS listeners secure_socket.c stashes the handler symbol via
         * an explicit listen path; reaching here means the listener was
         * created without one — bail loudly so this never silently
         * downgrades to plain TCP. */
        cando_vm_error(vm,
            "server:listen: TLS listener created without TLS handler");
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        return -1;
    }

    AcceptArg *aarg = (AcceptArg *)malloc(sizeof(AcceptArg));
    if (!aarg) {
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        cando_vm_error(vm, "server:listen: out of memory");
        return -1;
    }
    int sidx = (int)(s - g_socket_pool);
    aarg->listener_idx = sidx;
    aarg->handler      = handler;

    if (!cando_os_thread_create(&s->accept_thread, accept_thread_fn, aarg)) {
        free(aarg);
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        cando_vm_error(vm, "server:listen: failed to start accept thread");
        return -1;
    }
    s->has_accept_thread = true;

    /* Acquire a VM lifeline so the script doesn't have to await on
     * the listener -- the process stays alive until close() (which
     * joins the accept thread, releasing the lifeline) or app.quit()
     * (which trips cando_vm_quit_requested in the loop above). */
    cando_vm_lifeline_acquire(vm, "tcp_listener");
    s->has_lifeline = true;

    cando_vm_push(vm, args[0]);
    return 1;
}

/* tcp_server:close() — stops the accept loop and releases the listener slot. */
static int tcp_server_close_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    atomic_store(&s->running, false);
    if (s->fd != SOCKUTIL_INVALID_SOCKET) {
        sockutil_shutdown(s->fd);
    }
    if (s->has_accept_thread) {
        cando_os_thread_join(s->accept_thread);
        s->has_accept_thread = false;
    }

    int sidx = (int)(s - g_socket_pool);
    socket_pool_release(sidx);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* tcp_server:fd() -> number */
static int tcp_server_fd_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    cando_vm_push(vm, cando_number(s ? (f64)s->fd : -1.0));
    return 1;
}

/* tcp_server:localAddress() -> { host, port, family } | null */
static int tcp_server_localAddress_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || s->fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    struct sockaddr_storage sa;
    socklen_t len = 0;
    if (!sockutil_get_local_addr(s->fd, &sa, &len)) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    return push_addr_object(vm, &sa, len);
}

void socket_meta_define_server_common(CandoVM *vm, CdoObject *tbl)
{
    if (!tbl) return;
    /* `:listen` is intentionally NOT registered here: TLS listeners need a
     * different implementation that runs the per-connection handshake on
     * the worker thread.  socket.c registers tcp_listen_fn on
     * `_meta.tcp_server`; secure_socket.c registers its own handler on
     * `_meta.tls_server`. */
    cando_lib_meta_define(vm, tbl, "close",        tcp_server_close_fn);
    cando_lib_meta_define(vm, tbl, "fd",           tcp_server_fd_fn);
    cando_lib_meta_define(vm, tbl, "localAddress", tcp_server_localAddress_fn);
}

/* =========================================================================
 * Module-level functions: socket.tcp / .connect / .createServer / .resolve
 * ===================================================================== */

/* socket.tcp() -> tcp_socket  (unconnected, unbound). */
static int mod_tcp_fn(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    int idx = socket_pool_alloc(SOCK_KIND_TCP);
    if (idx < 0) {
        cando_vm_error(vm, "socket.tcp: too many active sockets");
        return -1;
    }
    cando_vm_push(vm, socket_create_instance(vm, idx, "tcp_socket"));
    return 1;
}

/*
 * socket.connect(host, port [, opts]) -> tcp_socket
 *
 * Convenience wrapper that allocates a TCP slot, opens the connection, and
 * returns a ready-to-use socket.  `opts.timeout` (ms) and `opts.family`
 * ("inet" / "inet6" / "any") are honoured.
 */
static int mod_connect_fn(CandoVM *vm, int argc, CandoValue *args)
{
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    if (!host) {
        cando_vm_error(vm, "socket.connect: host (string) required");
        return -1;
    }
    int port = (int)libutil_arg_num_at(args, argc, 1, -1);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "socket.connect: invalid port");
        return -1;
    }
    int timeout = 0;
    int family  = AF_UNSPEC;
    if (argc >= 3) read_connect_opts(vm, args[2], &timeout, &family);

    char errbuf[160] = {0};
    sockutil_socket_t fd = sockutil_tcp_connect(host, port, family,
                                                timeout, errbuf, sizeof(errbuf));
    if (fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_error(vm, "socket.connect: %s",
                       errbuf[0] ? errbuf : "connect failed");
        return -1;
    }

    int idx = socket_pool_alloc(SOCK_KIND_TCP);
    if (idx < 0) {
        sockutil_close(fd);
        cando_vm_error(vm, "socket.connect: too many active sockets");
        return -1;
    }
    SocketSlot *s = socket_pool_get(idx);
    s->fd         = fd;
    s->connected  = true;
    s->timeout_ms = timeout;
    cando_vm_push(vm, socket_create_instance(vm, idx, "tcp_socket"));
    return 1;
}

/* socket.createServer(callback) -> tcp_server */
static int mod_createServer_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm,
            "socket.createServer: callback (function) required");
        return -1;
    }
    int idx = socket_pool_alloc(SOCK_KIND_TCP_LISTENER);
    if (idx < 0) {
        cando_vm_error(vm, "socket.createServer: too many active sockets");
        return -1;
    }
    SocketSlot *s = socket_pool_get(idx);
    s->parent_vm   = vm;
    s->callback_fn = cando_value_copy(args[0]);
    cando_vm_push(vm, socket_create_instance(vm, idx, "tcp_server"));
    return 1;
}

/* socket.resolve(host) -> array of address strings (IPv4 + IPv6). */
static int mod_resolve_fn(CandoVM *vm, int argc, CandoValue *args)
{
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    if (!host) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    char addrs[16][INET6_ADDRSTRLEN];
    int n = sockutil_resolve(host, AF_UNSPEC, addrs, 16);
    if (n <= 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue arr_val = cando_bridge_new_array(vm);
    CdoObject *arr     = cando_bridge_resolve(vm, cando_as_handle(arr_val));
    for (int i = 0; i < n; i++) {
        CdoString *cs = cdo_string_new(addrs[i], (u32)strlen(addrs[i]));
        cdo_array_push(arr, cdo_string_value(cs));
        cdo_string_release(cs);
    }
    cando_vm_push(vm, arr_val);
    return 1;
}

/* =========================================================================
 * Registration
 * ===================================================================== */

void cando_lib_socket_register(CandoVM *vm)
{
    sockutil_one_time_init();
    ensure_pool_inited();
    cando_lib_meta_register(vm);

    /* Module globals. */
    CandoValue mod_val = cando_bridge_new_object(vm);
    CdoObject *mod_obj = cando_bridge_resolve(vm, cando_as_handle(mod_val));
    libutil_set_method(vm, mod_obj, "tcp",          mod_tcp_fn);
    libutil_set_method(vm, mod_obj, "connect",      mod_connect_fn);
    libutil_set_method(vm, mod_obj, "createServer", mod_createServer_fn);
    libutil_set_method(vm, mod_obj, "resolve",      mod_resolve_fn);
    cando_vm_set_global(vm, "socket", mod_val, true);

    /* Meta tables. */
    CdoObject *tcp_sock_meta = cando_lib_meta_table(vm, "tcp_socket");
    socket_meta_define_common(vm, tcp_sock_meta);

    CdoObject *tcp_srv_meta  = cando_lib_meta_table(vm, "tcp_server");
    socket_meta_define_server_common(vm, tcp_srv_meta);
    cando_lib_meta_define(vm, tcp_srv_meta, "listen", tcp_listen_fn);
}
