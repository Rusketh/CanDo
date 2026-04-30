/*
 * lib/secure_socket.c -- TLS variant of the `socket` library.
 *
 * Reuses the shared SocketSlot pool and metatable scaffolding declared in
 * socket.h.  Only the TLS-specific bits (handshake, peerCertificate, cipher,
 * protocol, server accept-with-handshake) live here.
 *
 * Layered structure:
 *   1. Field-reader helpers for parsing the opts object.
 *   2. TLS-only connection methods (peerCertificate, cipher, protocol,
 *      handshake) registered on `_meta.tls_socket`.
 *   3. TLS server accept loop: per connection, allocates a TLS slot,
 *      runs SSL_accept, then dispatches into the user callback.
 *   4. Module-level natives: secure_socket.tcp, .connect, .createServer.
 *   5. Registration entry point.
 *
 * Must compile with gcc -std=c11.
 */

#include "secure_socket.h"
#include "socket.h"
#include "sockutil.h"
#include "libutil.h"
#include "meta.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/string.h"
#include "../core/thread_platform.h"

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Field readers (small subset, modelled on http.c's helpers).
 *
 * The opts object is a plain CdoObject; we only need string and bool
 * lookups.  Unlike http.c we explicitly distinguish "field absent" from
 * "field set to null" so verifyPeer's true-by-default semantics hold.
 * ===================================================================== */

static const char *opts_get_str(CdoObject *opts, const char *name, u32 *len)
{
    *len = 0;
    if (!opts) return NULL;
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(opts, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_STRING || !v.as.string) return NULL;
    *len = v.as.string->length;
    return v.as.string->data;
}

/*
 * opts_get_bool -- returns true if the field exists and is bool-true.
 * `default_val` is returned when the field is absent or non-bool.
 */
static bool opts_get_bool(CdoObject *opts, const char *name, bool default_val)
{
    if (!opts) return default_val;
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(opts, key, &v);
    cdo_string_release(key);
    if (!ok) return default_val;
    if (v.tag == CDO_BOOL)   return v.as.boolean;
    if (v.tag == CDO_NUMBER) return v.as.number != 0.0;
    return default_val;
}

static int opts_get_int(CdoObject *opts, const char *name, int default_val)
{
    if (!opts) return default_val;
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(opts, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_NUMBER) return default_val;
    return (int)v.as.number;
}

static int parse_family(const char *s)
{
    if (!s || !*s) return AF_UNSPEC;
    if (strcmp(s, "inet")  == 0) return AF_INET;
    if (strcmp(s, "inet6") == 0) return AF_INET6;
    return AF_UNSPEC;
}

/*
 * Populate a SockutilTlsClientOpts struct from a CanDo opts object.  The
 * returned struct borrows pointers from the opts object's string fields
 * — keep `opts` alive while the struct is in use.  Sets verify_peer to
 * `true` when no explicit value is present (the new-API default).
 */
static void fill_client_opts(CdoObject *opts, SockutilTlsClientOpts *out)
{
    memset(out, 0, sizeof(*out));
    out->verify_peer = opts_get_bool(opts, "verifyPeer", true);
    out->ca_pem      = opts_get_str(opts, "ca",   &out->ca_pem_len);
    out->cert_pem    = opts_get_str(opts, "cert", &out->cert_pem_len);
    out->key_pem     = opts_get_str(opts, "key",  &out->key_pem_len);
}

/* =========================================================================
 * TLS-only connection methods (registered on _meta.tls_socket alongside
 * the shared methods from socket_meta_define_common).
 * ===================================================================== */

/* tls_socket:cipher() -> string | null */
static int tls_cipher_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || !s->ssl) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    const char *name = SSL_get_cipher_name(s->ssl);
    if (!name) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    libutil_push_cstr(vm, name);
    return 1;
}

/* tls_socket:protocol() -> string  (e.g. "TLSv1.3") */
static int tls_protocol_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || !s->ssl) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    const char *name = SSL_get_version(s->ssl);
    if (!name) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    libutil_push_cstr(vm, name);
    return 1;
}

/*
 * Helper: write a value into an object under key `name`.  Used to populate
 * the peerCertificate object.
 */
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

/*
 * tls_socket:peerCertificate() -> { subject, issuer, notBefore, notAfter,
 *                                   fingerprint } | null
 *
 * The fingerprint is the SHA-256 digest of the DER encoding, encoded as
 * lowercase hex with no separators (matches the format every browser and
 * CLI uses for cert pins).
 */
static int tls_peerCertificate_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm, argc > 0 ? args[0] : cando_null());
    if (!s || !s->ssl) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    X509 *cert = SSL_get_peer_certificate(s->ssl);
    if (!cert) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, obj_val.as.handle);

    /* subject + issuer as RFC 2253-ish "/CN=foo/O=bar" strings (the OpenSSL
     * default formatting suffices for inspection). */
    char buf[512];
    X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
    set_str_field(obj, "subject", buf, (u32)strlen(buf));

    X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof(buf));
    set_str_field(obj, "issuer", buf, (u32)strlen(buf));

    /* notBefore / notAfter as ISO-ish strings via ASN1_TIME_print. */
    BIO *mem = BIO_new(BIO_s_mem());
    if (mem) {
        ASN1_TIME_print(mem, X509_get0_notBefore(cert));
        char *p = NULL;
        long  n = BIO_get_mem_data(mem, &p);
        set_str_field(obj, "notBefore", p, (u32)n);
        BIO_free(mem);
    }
    mem = BIO_new(BIO_s_mem());
    if (mem) {
        ASN1_TIME_print(mem, X509_get0_notAfter(cert));
        char *p = NULL;
        long  n = BIO_get_mem_data(mem, &p);
        set_str_field(obj, "notAfter", p, (u32)n);
        BIO_free(mem);
    }

    /* SHA-256 fingerprint of the DER form. */
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int  dlen = sizeof(digest);
    if (X509_digest(cert, EVP_sha256(), digest, &dlen) == 1) {
        char hex[SHA256_DIGEST_LENGTH * 2 + 1];
        for (unsigned int i = 0; i < dlen; i++)
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        set_str_field(obj, "fingerprint", hex, dlen * 2);
    }

    X509_free(cert);
    cando_vm_push(vm, obj_val);
    return 1;
}

/* =========================================================================
 * TLS server: per-connection handler with handshake before user callback
 * ===================================================================== */

static void secure_socket_tls_conn_handler(int listener_idx,
                                           sockutil_socket_t cfd)
{
    SocketSlot *listener = socket_pool_get(listener_idx);
    if (!listener || !listener->ssl_ctx) {
        sockutil_close(cfd);
        return;
    }

    /* Allocate a TLS connection slot so the user callback's :cipher /
     * :peerCertificate methods see a valid SSL handle. */
    int conn_idx = socket_pool_alloc(SOCK_KIND_TLS);
    if (conn_idx < 0) {
        sockutil_close(cfd);
        return;
    }
    SocketSlot *conn = socket_pool_get(conn_idx);
    conn->fd = cfd;

    /* Run the SSL_accept handshake on the worker thread before we hand the
     * connection off to script code.  On failure, clean up quietly — there
     * is no way to surface an error to the script without a callback for
     * the listener (kept simple in v1). */
    char err[160] = {0};
    SSL *ssl = sockutil_tls_wrap(cfd, listener->ssl_ctx, false, NULL,
                                 err, sizeof(err));
    if (!ssl) {
        socket_pool_release(conn_idx);
        return;
    }
    conn->ssl          = ssl;
    conn->ssl_ctx      = listener->ssl_ctx;  /* shared, NOT owned by conn */
    conn->owns_ssl_ctx = false;
    conn->connected    = true;

    /* Spin up a child VM and dispatch the user callback with the conn
     * stamped as `_meta.tls_socket`. */
    CandoVM child;
    cando_vm_init_child(&child, listener->parent_vm);

    CandoValue conn_val = socket_create_instance(&child, conn_idx, "tls_socket");

    CandoValue cb_args[1] = { conn_val };
    cando_vm_call_value(&child, listener->callback_fn, cb_args, 1);

    socket_pool_release(conn_idx);
    cando_vm_destroy(&child);
}

/*
 * Custom listen for TLS servers.  Same shape as socket.c's tcp_listen_fn
 * but installs the TLS handler.  Lives here (not in socket.c) so the TLS
 * connection-handler symbol stays internal to secure_socket.c.
 *
 * Forward-declared accept thread / arg pair are duplicated to avoid
 * exporting the structures from socket.c.  The accept loop body is shared
 * via socket_run_accept_loop, which spins on listener->running and
 * dispatches to the supplied per-connection handler.
 */
typedef struct AcceptArg {
    int               listener_idx;
    SocketConnHandler handler;
} AcceptArg;

static CANDO_THREAD_RETURN tls_accept_thread_fn(void *arg_p)
{
    AcceptArg *arg = (AcceptArg *)arg_p;
    int                idx     = arg->listener_idx;
    SocketConnHandler  handler = arg->handler;
    free(arg);
    socket_run_accept_loop(idx, handler);
    return CANDO_THREAD_RETURN_VAL;
}

static int tls_listen_fn(CandoVM *vm, int argc, CandoValue *args)
{
    SocketSlot *s = socket_resolve_receiver(vm,
                                            argc > 0 ? args[0] : cando_null());
    if (!s || s->kind != SOCK_KIND_TLS_LISTENER) {
        cando_vm_error(vm, "tls_server:listen: invalid receiver");
        return -1;
    }
    int port = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "tls_server:listen: invalid port");
        return -1;
    }
    const char *host = libutil_arg_cstr_at(args, argc, 2);
    int backlog = (int)libutil_arg_num_at(args, argc, 3, 64);

    if (cando_is_null(s->callback_fn)) {
        cando_vm_error(vm,
            "tls_server:listen: no connection callback was set");
        return -1;
    }

    char errbuf[160] = {0};
    sockutil_socket_t fd = sockutil_tcp_listen(host, port, AF_UNSPEC,
                                               backlog, errbuf, sizeof(errbuf));
    if (fd == SOCKUTIL_INVALID_SOCKET) {
        cando_vm_error(vm, "tls_server:listen: %s",
                       errbuf[0] ? errbuf : "bind/listen failed");
        return -1;
    }
    s->fd = fd;
    atomic_store(&s->running, true);

    AcceptArg *aarg = (AcceptArg *)malloc(sizeof(AcceptArg));
    if (!aarg) {
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        cando_vm_error(vm, "tls_server:listen: out of memory");
        return -1;
    }
    /* Slot index recovered from the SocketSlot pointer in the public API
     * is not exposed; round-trip via the receiver instead. */
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    CdoString *key = cdo_string_intern("__socket_id", 11);
    CdoValue   v   = cdo_null();
    bool ok        = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_NUMBER) {
        free(aarg);
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        cando_vm_error(vm, "tls_server:listen: receiver lost __socket_id");
        return -1;
    }
    aarg->listener_idx = (int)v.as.number;
    aarg->handler      = secure_socket_tls_conn_handler;

    if (!cando_os_thread_create(&s->accept_thread, tls_accept_thread_fn, aarg)) {
        free(aarg);
        atomic_store(&s->running, false);
        sockutil_close(fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
        cando_vm_error(vm, "tls_server:listen: failed to start accept thread");
        return -1;
    }
    s->has_accept_thread = true;

    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Module-level natives: secure_socket.tcp / .connect / .createServer
 * ===================================================================== */

/*
 * secure_socket.tcp([opts]) -> tls_socket  (unconnected)
 *
 * The opts (verifyPeer / ca / cert / key / serverName) are stashed on the
 * slot so the subsequent `:connect()` can use them when wrapping the
 * freshly opened TCP fd with TLS.
 */
static int mod_tcp_fn(CandoVM *vm, int argc, CandoValue *args)
{
    int idx = socket_pool_alloc(SOCK_KIND_TLS);
    if (idx < 0) {
        cando_vm_error(vm, "secure_socket.tcp: too many active sockets");
        return -1;
    }
    SocketSlot *s = socket_pool_get(idx);

    if (argc >= 1 && cando_is_object(args[0])) {
        CdoObject *opts = cando_bridge_resolve(vm, args[0].as.handle);

        SockutilTlsClientOpts copts;
        fill_client_opts(opts, &copts);

        char err[160] = {0};
        SSL_CTX *ctx = sockutil_build_client_ssl_ctx(&copts, err, sizeof(err));
        if (!ctx) {
            socket_pool_release(idx);
            cando_vm_error(vm, "secure_socket.tcp: %s",
                           err[0] ? err : "SSL_CTX build failed");
            return -1;
        }
        s->ssl_ctx      = ctx;
        s->owns_ssl_ctx = true;

        u32 sn_len = 0;
        const char *sn = opts_get_str(opts, "serverName", &sn_len);
        if (sn && sn_len > 0) {
            s->sni_host = (char *)malloc(sn_len + 1);
            if (s->sni_host) {
                memcpy(s->sni_host, sn, sn_len);
                s->sni_host[sn_len] = '\0';
            }
        }
    }

    cando_vm_push(vm, socket_create_instance(vm, idx, "tls_socket"));
    return 1;
}

/*
 * secure_socket.connect(host, port [, opts]) -> tls_socket
 *
 * One-shot helper: builds the client SSL_CTX (verifyPeer defaults to true),
 * opens the TCP connection, runs the TLS handshake, and returns the wrapped
 * socket ready for I/O.
 */
static int mod_connect_fn(CandoVM *vm, int argc, CandoValue *args)
{
    const char *host = libutil_arg_cstr_at(args, argc, 0);
    if (!host) {
        cando_vm_error(vm, "secure_socket.connect: host (string) required");
        return -1;
    }
    int port = (int)libutil_arg_num_at(args, argc, 1, -1);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "secure_socket.connect: invalid port");
        return -1;
    }

    CdoObject *opts = NULL;
    if (argc >= 3 && cando_is_object(args[2])) {
        opts = cando_bridge_resolve(vm, args[2].as.handle);
    }

    int timeout = opts ? opts_get_int(opts, "timeout", 0) : 0;
    int family  = AF_UNSPEC;
    if (opts) {
        u32 fam_len = 0;
        const char *fam = opts_get_str(opts, "family", &fam_len);
        if (fam) family = parse_family(fam);
    }

    SockutilTlsClientOpts copts;
    fill_client_opts(opts, &copts);

    /* Build client CTX before opening the socket so we can fail fast on
     * malformed PEMs without leaving an open fd dangling. */
    char err[160] = {0};
    SSL_CTX *ctx = sockutil_build_client_ssl_ctx(&copts, err, sizeof(err));
    if (!ctx) {
        cando_vm_error(vm, "secure_socket.connect: %s",
                       err[0] ? err : "SSL_CTX build failed");
        return -1;
    }

    sockutil_socket_t fd = sockutil_tcp_connect(host, port, family, timeout,
                                                err, sizeof(err));
    if (fd == SOCKUTIL_INVALID_SOCKET) {
        SSL_CTX_free(ctx);
        cando_vm_error(vm, "secure_socket.connect: %s",
                       err[0] ? err : "connect failed");
        return -1;
    }

    /* SNI host: explicit serverName overrides the connect host. */
    const char *sni = host;
    if (opts) {
        u32 sn_len = 0;
        const char *sn = opts_get_str(opts, "serverName", &sn_len);
        if (sn && sn_len > 0) sni = sn;  /* points into opts; safe for the call */
    }

    SSL *ssl = sockutil_tls_wrap(fd, ctx, true, sni, err, sizeof(err));
    if (!ssl) {
        sockutil_close(fd);
        SSL_CTX_free(ctx);
        cando_vm_error(vm, "secure_socket.connect: %s",
                       err[0] ? err : "TLS handshake failed");
        return -1;
    }

    int idx = socket_pool_alloc(SOCK_KIND_TLS);
    if (idx < 0) {
        sockutil_tls_free(ssl);
        sockutil_close(fd);
        SSL_CTX_free(ctx);
        cando_vm_error(vm, "secure_socket.connect: too many active sockets");
        return -1;
    }
    SocketSlot *s    = socket_pool_get(idx);
    s->fd            = fd;
    s->ssl           = ssl;
    s->ssl_ctx       = ctx;
    s->owns_ssl_ctx  = true;
    s->connected     = true;
    s->timeout_ms    = timeout;

    cando_vm_push(vm, socket_create_instance(vm, idx, "tls_socket"));
    return 1;
}

/*
 * secure_socket.createServer(opts, callback) -> tls_server
 *
 * `opts.cert` and `opts.key` are required PEM strings.  `opts.verifyPeer`
 * (default false on the server) plus `opts.ca` enable client-cert
 * verification.
 */
static int mod_createServer_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_object(args[1])) {
        cando_vm_error(vm,
            "secure_socket.createServer: expected (opts, callback)");
        return -1;
    }
    CdoObject *opts = cando_bridge_resolve(vm, args[0].as.handle);

    u32 cert_len = 0, key_len = 0;
    const char *cert = opts_get_str(opts, "cert", &cert_len);
    const char *key  = opts_get_str(opts, "key",  &key_len);
    if (!cert || !key) {
        cando_vm_error(vm,
            "secure_socket.createServer: opts.cert and opts.key are required");
        return -1;
    }

    SockutilTlsServerOpts sopts;
    memset(&sopts, 0, sizeof(sopts));
    sopts.verify_peer = opts_get_bool(opts, "verifyPeer", false);
    sopts.ca_pem      = opts_get_str(opts, "ca", &sopts.ca_pem_len);

    char err[160] = {0};
    SSL_CTX *ctx = sockutil_build_server_ssl_ctx(cert, cert_len, key, key_len,
                                                 &sopts, err, sizeof(err));
    if (!ctx) {
        cando_vm_error(vm, "secure_socket.createServer: %s",
                       err[0] ? err : "SSL_CTX build failed");
        return -1;
    }

    int idx = socket_pool_alloc(SOCK_KIND_TLS_LISTENER);
    if (idx < 0) {
        SSL_CTX_free(ctx);
        cando_vm_error(vm,
            "secure_socket.createServer: too many active sockets");
        return -1;
    }
    SocketSlot *s   = socket_pool_get(idx);
    s->parent_vm    = vm;
    s->callback_fn  = cando_value_copy(args[1]);
    s->ssl_ctx      = ctx;
    s->owns_ssl_ctx = true;

    cando_vm_push(vm, socket_create_instance(vm, idx, "tls_server"));
    return 1;
}

/* =========================================================================
 * Registration
 * ===================================================================== */

void cando_lib_secure_socket_register(CandoVM *vm)
{
    sockutil_one_time_init();
    cando_lib_meta_register(vm);

    CandoValue mod_val = cando_bridge_new_object(vm);
    CdoObject *mod_obj = cando_bridge_resolve(vm, mod_val.as.handle);
    libutil_set_method(vm, mod_obj, "tcp",          mod_tcp_fn);
    libutil_set_method(vm, mod_obj, "connect",      mod_connect_fn);
    libutil_set_method(vm, mod_obj, "createServer", mod_createServer_fn);
    cando_vm_set_global(vm, "secure_socket", mod_val, true);

    /* Connection metatable: shared methods plus TLS introspection. */
    CdoObject *tls_sock = cando_lib_meta_table(vm, "tls_socket");
    socket_meta_define_common(vm, tls_sock);
    cando_lib_meta_define(vm, tls_sock, "cipher",          tls_cipher_fn);
    cando_lib_meta_define(vm, tls_sock, "protocol",        tls_protocol_fn);
    cando_lib_meta_define(vm, tls_sock, "peerCertificate", tls_peerCertificate_fn);

    /* Listener metatable: shared close/fd/localAddress plus TLS-aware listen. */
    CdoObject *tls_srv = cando_lib_meta_table(vm, "tls_server");
    socket_meta_define_server_common(vm, tls_srv);
    cando_lib_meta_define(vm, tls_srv, "listen", tls_listen_fn);
}
