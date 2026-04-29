/*
 * lib/socket.h -- Raw TCP socket standard library for CanDo.
 *
 * Registers a global `socket` object with:
 *   socket.tcp()                                -- unconnected TCP socket
 *   socket.connect(host, port [, opts])         -- tcp() + :connect() helper
 *   socket.createServer(callback)               -- non-blocking TCP server
 *   socket.resolve(host)                        -- IPv4 + IPv6 address list
 *
 * Plus the user-extensible meta-tables:
 *   _meta.tcp_socket   -- per-connection methods
 *   _meta.tcp_server   -- listener methods
 *
 * Server callback semantics
 * ─────────────────────────
 * `socket.createServer(callback)` mirrors `http.createServer` exactly:
 *   - `:listen(port)` returns immediately and spawns a background accept
 *     thread; the calling script is *not* blocked.
 *   - For each accepted connection a fresh child VM thread runs
 *     `callback(conn)`.  The callback owns the connection's lifetime; when
 *     it returns, the underlying socket is closed and the pool slot freed.
 *   - Inside the callback, blocking `:recv`/`:sendAll` block only that
 *     connection's worker thread.
 *
 * Errors
 * ──────
 * Programmer mistakes (wrong types, calling `:send` on a closed socket,
 * unknown options) and I/O failures (refused connect, peer reset, timeout,
 * bind failure) all throw via `cando_vm_error`.  A clean EOF on `:recv`
 * returns the empty string (no error).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_SOCKET_H
#define CANDO_LIB_SOCKET_H

#include "../vm/vm.h"
#include "../object/object.h"
#include "../core/thread_platform.h"

#include "sockutil.h"

#include <stdatomic.h>

/* =========================================================================
 * Public registration
 * ===================================================================== */

CANDO_API void cando_lib_socket_register(CandoVM *vm);

/* =========================================================================
 * Internal pool API exposed for `secure_socket` to reuse.
 *
 * Both libraries share one pool because a TLS socket is a TCP socket plus an
 * `SSL *`; carrying two pools would duplicate every alloc/free path with no
 * benefit.  `secure_socket.c` allocates slots with the TLS kinds and fills
 * the `ssl` / `ssl_ctx` fields after the handshake.
 * ===================================================================== */

#define SOCKET_MAX_INSTANCES 256

typedef enum SocketKind {
    SOCK_KIND_UNUSED = 0,
    SOCK_KIND_TCP,            /* connected or unconnected TCP socket   */
    SOCK_KIND_TCP_LISTENER,
    SOCK_KIND_TLS,
    SOCK_KIND_TLS_LISTENER,
} SocketKind;

typedef struct SocketSlot {
    /* Identity */
    bool                 in_use;
    SocketKind           kind;

    /* Transport */
    sockutil_socket_t    fd;
    bool                 connected;
    int                  timeout_ms;

    /* TLS (used by secure_socket only; NULL for plain TCP) */
    SSL                 *ssl;
    SSL_CTX             *ssl_ctx;
    bool                 owns_ssl_ctx;
    /* Stored SNI / verify hostname for client TLS handshakes initiated from
     * an unconnected `secure_socket.tcp({...})` followed by `:connect()`.
     * Owned by the slot. */
    char                *sni_host;

    /* Listener-only */
    cando_thread_t       accept_thread;
    bool                 has_accept_thread;
    bool                 has_lifeline;     /* held while listener is alive */
    _Atomic(bool)        running;
    CandoVM             *parent_vm;
    CandoValue           callback_fn;
} SocketSlot;

/*
 * socket_pool_alloc -- reserve and zero-initialise a slot of `kind`.
 * Returns the slot index in [0, SOCKET_MAX_INSTANCES) or -1 on exhaustion.
 */
int  socket_pool_alloc(SocketKind kind);

/*
 * socket_pool_get -- index → SocketSlot*.  Returns NULL if `idx` is out of
 * range or the slot is no longer in use.
 */
SocketSlot *socket_pool_get(int idx);

/*
 * socket_pool_release -- close any held resources (SSL, fd) and mark the
 * slot reusable.  Idempotent.
 */
void socket_pool_release(int idx);

/*
 * socket_resolve_receiver -- given the first VM arg (the receiver of a
 * method call), recover the underlying SocketSlot.  Returns NULL if the
 * receiver is not a socket or its slot has been released.
 */
SocketSlot *socket_resolve_receiver(CandoVM *vm, CandoValue receiver);

/*
 * socket_meta_define_common -- populate the connection-level methods
 * (connect/send/recv/close/etc.) on the given meta table.  Used by both
 * `_meta.tcp_socket` (in socket.c) and `_meta.tls_socket` (in
 * secure_socket.c) so the two surfaces never drift.
 */
void socket_meta_define_common(CandoVM *vm, CdoObject *tbl);

/*
 * socket_meta_define_server_common -- populate the listener-level methods
 * (listen/close/localAddress/etc.).  Used by both `_meta.tcp_server` and
 * `_meta.tls_server`.
 */
void socket_meta_define_server_common(CandoVM *vm, CdoObject *tbl);

/*
 * socket_create_instance -- allocate a fresh CdoObject, stamp `__socket_id`
 * and attach the named meta table.  Returns the CandoValue holding the new
 * object.
 */
CandoValue socket_create_instance(CandoVM *vm, int slot_idx,
                                  const char *meta_name);

/*
 * socket_run_accept_loop -- the body of the listener's accept thread,
 * invoked indirectly by `:listen`.  Exposed so `secure_socket.c` can wrap
 * the per-connection child-VM setup with its TLS handshake while reusing
 * the same accept loop.
 *
 * The function blocks until the listener stops; do not call it directly
 * from script-facing code.
 */
typedef void (*SocketConnHandler)(int listener_idx, sockutil_socket_t cfd);
void socket_run_accept_loop(int listener_idx, SocketConnHandler handler);

/*
 * socket_default_conn_handler -- the plain-TCP handler used by `socket.c`.
 * Allocates a TCP slot, builds the conn object, invokes the user callback
 * inside a child VM, then frees the slot.  Exposed so secure_socket.c can
 * wrap it with a TLS-handshake step.
 */
void socket_default_conn_handler(int listener_idx, sockutil_socket_t cfd);

#endif /* CANDO_LIB_SOCKET_H */
