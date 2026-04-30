/*
 * lib/sockutil.c -- Implementation of the cross-platform TCP+TLS primitives
 *                   declared in sockutil.h.
 *
 * Layered structure:
 *   1. Platform shims (CLOSESOCK, errno, sockaddr typedefs).
 *   2. One-time init (WSAStartup + SIGPIPE + OpenSSL).
 *   3. Address resolution / connect / listen / accept.
 *   4. Socket option helpers.
 *   5. Address introspection.
 *   6. Raw byte I/O.
 *   7. TLS context construction and wrapping.
 *
 * Must compile with gcc -std=c11.
 */

#include "sockutil.h"

#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#if defined(CANDO_PLATFORM_WINDOWS)
#  define CLOSESOCK(fd) closesocket(fd)
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/time.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  define CLOSESOCK(fd) close(fd)
#endif

/* =========================================================================
 * Internal helpers
 * ===================================================================== */

/* Format an error message into the optional `err` buffer.  Safe to call
 * with err == NULL (in which case the call is a no-op). */
static void seterr(char *err, usize errlen, const char *fmt, ...)
{
    if (!err || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * One-time global init
 * ===================================================================== */

static _Atomic(int) g_init_done = 0;

void sockutil_one_time_init(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_init_done, &expected, 1))
        return;

#if defined(CANDO_PLATFORM_WINDOWS)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    /* On POSIX a write to a peer that has closed the socket would raise
     * SIGPIPE and (by default) terminate the process.  TLS sends go through
     * SSL_write which masks this internally, but raw socket sends do not.
     * Ignoring the signal lets send() simply return -1/EPIPE so the calling
     * native can surface the error to the script. */
    signal(SIGPIPE, SIG_IGN);
#endif

    /* OpenSSL 1.1+ auto-initialises; this call is a no-op but harmless and
     * keeps us forward-compatible with explicit-init builds. */
    (void)OPENSSL_init_ssl(
        OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}

/* =========================================================================
 * TCP connect / listen / accept
 * ===================================================================== */

sockutil_socket_t sockutil_tcp_connect(const char *host, int port,
                                       int family, int timeout_ms,
                                       char *err, usize errlen)
{
    sockutil_one_time_init();

    if (!host || port <= 0 || port > 65535) {
        seterr(err, errlen, "invalid host/port");
        return SOCKUTIL_INVALID_SOCKET;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        seterr(err, errlen, "getaddrinfo failed: %s", gai_strerror(rc));
        return SOCKUTIL_INVALID_SOCKET;
    }

    sockutil_socket_t fd = SOCKUTIL_INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = (sockutil_socket_t)socket(ai->ai_family, ai->ai_socktype,
                                       ai->ai_protocol);
        if (fd < 0) {
            fd = SOCKUTIL_INVALID_SOCKET;
            continue;
        }
        sockutil_set_timeout(fd, timeout_ms);
        if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0)
            break;
        CLOSESOCK(fd);
        fd = SOCKUTIL_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (fd == SOCKUTIL_INVALID_SOCKET) {
        seterr(err, errlen, "connect failed: %s:%d", host, port);
    }
    return fd;
}

sockutil_socket_t sockutil_tcp_listen(const char *host, int port,
                                      int family, int backlog,
                                      char *err, usize errlen)
{
    sockutil_one_time_init();

    if (port < 0 || port > 65535) {
        seterr(err, errlen, "invalid port");
        return SOCKUTIL_INVALID_SOCKET;
    }
    if (backlog <= 0) backlog = 64;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    const char *node = (host && *host) ? host : NULL;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(node, portstr, &hints, &res);
    if (rc != 0 || !res) {
        seterr(err, errlen, "getaddrinfo failed: %s", gai_strerror(rc));
        return SOCKUTIL_INVALID_SOCKET;
    }

    sockutil_socket_t fd = SOCKUTIL_INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = (sockutil_socket_t)socket(ai->ai_family, ai->ai_socktype,
                                       ai->ai_protocol);
        if (fd < 0) {
            fd = SOCKUTIL_INVALID_SOCKET;
            continue;
        }
        sockutil_set_reuseaddr(fd, true);

#ifdef IPV6_V6ONLY
        if (ai->ai_family == AF_INET6) {
            /* Allow IPv4-mapped clients to use this listener too, so that
             * binding to "::" (or AF_UNSPEC default) catches both stacks. */
            int off = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char *)&off, sizeof(off));
        }
#endif

        if (bind(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0) {
            break;
        }
        CLOSESOCK(fd);
        fd = SOCKUTIL_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (fd == SOCKUTIL_INVALID_SOCKET) {
        seterr(err, errlen, "bind/listen failed on port %d", port);
    }
    return fd;
}

sockutil_socket_t sockutil_tcp_accept(sockutil_socket_t listen_fd,
                                      int timeout_ms,
                                      struct sockaddr_storage *peer_addr,
                                      socklen_t *peer_len,
                                      char *err, usize errlen)
{
    if (listen_fd < 0) {
        seterr(err, errlen, "invalid listener");
        return SOCKUTIL_INVALID_SOCKET;
    }

    /* The simplest portable timeout for accept is SO_RCVTIMEO on the listener
     * itself.  Setting it temporarily here is fine because the listener is
     * not used for any other operation. */
    if (timeout_ms > 0) sockutil_set_timeout(listen_fd, timeout_ms);

    struct sockaddr_storage local_sa;
    socklen_t               local_len = sizeof(local_sa);
    struct sockaddr_storage *out_sa  = peer_addr ? peer_addr : &local_sa;
    socklen_t               *out_len = peer_len  ? peer_len  : &local_len;
    *out_len = sizeof(*out_sa);

    sockutil_socket_t cfd = (sockutil_socket_t)accept(listen_fd,
                                                     (struct sockaddr *)out_sa,
                                                     out_len);
    if (cfd < 0) {
        seterr(err, errlen, "accept timed out or failed");
        return SOCKUTIL_INVALID_SOCKET;
    }
    return cfd;
}

/* =========================================================================
 * Socket lifecycle and options
 * ===================================================================== */

void sockutil_close(sockutil_socket_t fd)
{
    if (fd < 0) return;
    CLOSESOCK(fd);
}

void sockutil_shutdown(sockutil_socket_t fd)
{
    if (fd < 0) return;
#if defined(CANDO_PLATFORM_WINDOWS)
    shutdown(fd, SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}

void sockutil_set_timeout(sockutil_socket_t fd, int timeout_ms)
{
    if (fd < 0) return;
#if defined(CANDO_PLATFORM_WINDOWS)
    DWORD tv = (DWORD)(timeout_ms > 0 ? timeout_ms : 0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    if (timeout_ms <= 0) {
        tv.tv_sec  = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool sockutil_set_blocking(sockutil_socket_t fd, bool blocking)
{
    if (fd < 0) return false;
#if defined(CANDO_PLATFORM_WINDOWS)
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (blocking) flags &= ~O_NONBLOCK;
    else          flags |=  O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool sockutil_set_reuseaddr(sockutil_socket_t fd, bool yes)
{
    if (fd < 0) return false;
    int v = yes ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char *)&v, sizeof(v)) == 0;
}

bool sockutil_set_nodelay(sockutil_socket_t fd, bool yes)
{
    if (fd < 0) return false;
    int v = yes ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      (const char *)&v, sizeof(v)) == 0;
}

bool sockutil_set_keepalive(sockutil_socket_t fd, bool yes)
{
    if (fd < 0) return false;
    int v = yes ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                      (const char *)&v, sizeof(v)) == 0;
}

bool sockutil_set_recvbuf(sockutil_socket_t fd, int bytes)
{
    if (fd < 0 || bytes <= 0) return false;
    return setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                      (const char *)&bytes, sizeof(bytes)) == 0;
}

bool sockutil_set_sendbuf(sockutil_socket_t fd, int bytes)
{
    if (fd < 0 || bytes <= 0) return false;
    return setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                      (const char *)&bytes, sizeof(bytes)) == 0;
}

/* =========================================================================
 * Address introspection
 * ===================================================================== */

bool sockutil_get_local_addr(sockutil_socket_t fd,
                             struct sockaddr_storage *out, socklen_t *outlen)
{
    if (fd < 0 || !out || !outlen) return false;
    *outlen = sizeof(*out);
    return getsockname(fd, (struct sockaddr *)out, outlen) == 0;
}

bool sockutil_get_peer_addr(sockutil_socket_t fd,
                            struct sockaddr_storage *out, socklen_t *outlen)
{
    if (fd < 0 || !out || !outlen) return false;
    *outlen = sizeof(*out);
    return getpeername(fd, (struct sockaddr *)out, outlen) == 0;
}

bool sockutil_addr_to_string(const struct sockaddr_storage *sa, socklen_t len,
                             char *host_out, usize host_max,
                             int *port_out, int *family_out)
{
    if (!sa || !host_out || host_max == 0) return false;

    char buf[NI_MAXHOST];
    char svc[NI_MAXSERV];
    int rc = getnameinfo((const struct sockaddr *)sa, len,
                         buf, sizeof(buf), svc, sizeof(svc),
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) return false;

    usize n = strlen(buf);
    if (n >= host_max) n = host_max - 1;
    memcpy(host_out, buf, n);
    host_out[n] = '\0';

    if (port_out)   *port_out   = atoi(svc);
    if (family_out) *family_out = sa->ss_family;
    return true;
}

int sockutil_resolve(const char *host, int family,
                     char (*out)[INET6_ADDRSTRLEN], int max_out)
{
    sockutil_one_time_init();
    if (!host || !out || max_out <= 0) return 0;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;

    int n = 0;
    for (struct addrinfo *ai = res; ai && n < max_out; ai = ai->ai_next) {
        char tmp[NI_MAXHOST];
        if (getnameinfo(ai->ai_addr, (socklen_t)ai->ai_addrlen,
                        tmp, sizeof(tmp), NULL, 0, NI_NUMERICHOST) == 0) {
            usize tlen = strlen(tmp);
            if (tlen >= INET6_ADDRSTRLEN) tlen = INET6_ADDRSTRLEN - 1;
            memcpy(out[n], tmp, tlen);
            out[n][tlen] = '\0';
            n++;
        }
    }
    freeaddrinfo(res);
    return n;
}

/* =========================================================================
 * Raw I/O
 * ===================================================================== */

int sockutil_send_raw(sockutil_socket_t fd, const void *buf, int len)
{
    if (fd < 0 || len <= 0) return -1;
#if defined(CANDO_PLATFORM_WINDOWS)
    return send(fd, (const char *)buf, len, 0);
#else
    /* MSG_NOSIGNAL on Linux suppresses SIGPIPE per-call; on systems without
     * it we fall back to the global SIG_IGN installed at one-time init. */
#  ifdef MSG_NOSIGNAL
    return (int)send(fd, buf, (size_t)len, MSG_NOSIGNAL);
#  else
    return (int)send(fd, buf, (size_t)len, 0);
#  endif
#endif
}

int sockutil_recv_raw(sockutil_socket_t fd, void *buf, int len)
{
    if (fd < 0 || len <= 0) return -1;
#if defined(CANDO_PLATFORM_WINDOWS)
    return recv(fd, (char *)buf, len, 0);
#else
    return (int)recv(fd, buf, (size_t)len, 0);
#endif
}

/* =========================================================================
 * TLS context construction
 * ===================================================================== */

/*
 * Load a chain of certificates from a PEM string into `ctx`.  The first cert
 * becomes the leaf via SSL_CTX_use_certificate; subsequent certs are added
 * with SSL_CTX_add_extra_chain_cert.  Returns true on success.
 */
static bool ctx_use_cert_chain(SSL_CTX *ctx, const char *pem, u32 len,
                               char *err, usize errlen)
{
    BIO *bio = BIO_new_mem_buf(pem, (int)len);
    if (!bio) { seterr(err, errlen, "cert BIO failed"); return false; }

    X509 *first = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    if (!first) {
        BIO_free(bio);
        seterr(err, errlen, "invalid PEM certificate");
        return false;
    }
    if (SSL_CTX_use_certificate(ctx, first) != 1) {
        X509_free(first);
        BIO_free(bio);
        seterr(err, errlen, "SSL_CTX_use_certificate failed");
        return false;
    }
    X509_free(first);

    X509 *extra;
    while ((extra = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (SSL_CTX_add_extra_chain_cert(ctx, extra) != 1) {
            X509_free(extra);
            break;  /* non-fatal; we already have the leaf */
        }
        /* ownership transferred to ctx on success */
    }
    BIO_free(bio);
    return true;
}

/*
 * Load a private key (PEM) and bind it to ctx, then verify it matches the
 * configured certificate.
 */
static bool ctx_use_private_key(SSL_CTX *ctx, const char *pem, u32 len,
                                char *err, usize errlen)
{
    BIO *bio = BIO_new_mem_buf(pem, (int)len);
    if (!bio) { seterr(err, errlen, "key BIO failed"); return false; }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) { seterr(err, errlen, "invalid PEM private key"); return false; }

    if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
        EVP_PKEY_free(pkey);
        seterr(err, errlen, "SSL_CTX_use_PrivateKey failed");
        return false;
    }
    EVP_PKEY_free(pkey);

    if (SSL_CTX_check_private_key(ctx) != 1) {
        seterr(err, errlen, "cert/key mismatch");
        return false;
    }
    return true;
}

/*
 * Append the certificates inside a PEM bundle to ctx's verify-store.  Used
 * for both client trust roots (verify_peer) and server-side client-cert
 * verification.
 */
static bool ctx_add_ca_pem(SSL_CTX *ctx, const char *pem, u32 len,
                           char *err, usize errlen)
{
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if (!store) {
        seterr(err, errlen, "no cert store");
        return false;
    }

    BIO *bio = BIO_new_mem_buf(pem, (int)len);
    if (!bio) { seterr(err, errlen, "ca BIO failed"); return false; }

    X509 *cert;
    int   added = 0;
    while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (X509_STORE_add_cert(store, cert) == 1) added++;
        X509_free(cert);
    }
    BIO_free(bio);

    if (added == 0) {
        seterr(err, errlen, "no certs in CA bundle");
        return false;
    }
    return true;
}

SSL_CTX *sockutil_build_client_ssl_ctx(const SockutilTlsClientOpts *opts,
                                       char *err, usize errlen)
{
    sockutil_one_time_init();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { seterr(err, errlen, "SSL_CTX_new failed"); return NULL; }

    if (opts && opts->verify_peer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(ctx);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    if (opts && opts->ca_pem && opts->ca_pem_len > 0) {
        if (!ctx_add_ca_pem(ctx, opts->ca_pem, opts->ca_pem_len, err, errlen)) {
            SSL_CTX_free(ctx);
            return NULL;
        }
    }

    if (opts && opts->cert_pem && opts->cert_pem_len > 0) {
        if (!ctx_use_cert_chain(ctx, opts->cert_pem, opts->cert_pem_len,
                                err, errlen)) {
            SSL_CTX_free(ctx);
            return NULL;
        }
        if (opts->key_pem && opts->key_pem_len > 0) {
            if (!ctx_use_private_key(ctx, opts->key_pem, opts->key_pem_len,
                                     err, errlen)) {
                SSL_CTX_free(ctx);
                return NULL;
            }
        }
    }

    return ctx;
}

SSL_CTX *sockutil_build_server_ssl_ctx(const char *cert_pem, u32 cert_len,
                                       const char *key_pem,  u32 key_len,
                                       const SockutilTlsServerOpts *opts,
                                       char *err, usize errlen)
{
    sockutil_one_time_init();

    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        seterr(err, errlen, "cert and key are required");
        return NULL;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { seterr(err, errlen, "SSL_CTX_new failed"); return NULL; }

    if (!ctx_use_cert_chain(ctx, cert_pem, cert_len, err, errlen)) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (!ctx_use_private_key(ctx, key_pem, key_len, err, errlen)) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (opts && opts->verify_peer) {
        SSL_CTX_set_verify(ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           NULL);
        if (opts->ca_pem && opts->ca_pem_len > 0) {
            if (!ctx_add_ca_pem(ctx, opts->ca_pem, opts->ca_pem_len,
                                err, errlen)) {
                SSL_CTX_free(ctx);
                return NULL;
            }
        }
    }

    return ctx;
}

SSL *sockutil_tls_wrap(sockutil_socket_t fd, SSL_CTX *ctx,
                       bool is_client, const char *sni_host,
                       char *err, usize errlen)
{
    if (fd < 0 || !ctx) {
        seterr(err, errlen, "invalid arguments");
        return NULL;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        seterr(err, errlen, "SSL_new failed");
        return NULL;
    }

    if (is_client && sni_host && *sni_host) {
        SSL_set_tlsext_host_name(ssl, sni_host);

        /* Enable hostname verification when the underlying ctx was built with
         * verify_peer=true.  This is a no-op (and safe) when verify mode is
         * SSL_VERIFY_NONE. */
        X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
        if (param) {
            X509_VERIFY_PARAM_set_hostflags(param,
                X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            X509_VERIFY_PARAM_set1_host(param, sni_host, 0);
        }
    }

    SSL_set_fd(ssl, fd);

    int rc = is_client ? SSL_connect(ssl) : SSL_accept(ssl);
    if (rc <= 0) {
        unsigned long e = ERR_peek_last_error();
        char openssl_buf[256] = {0};
        if (e) ERR_error_string_n(e, openssl_buf, sizeof(openssl_buf));
        seterr(err, errlen, "TLS handshake failed%s%s",
               openssl_buf[0] ? ": " : "",
               openssl_buf[0] ? openssl_buf : "");
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

void sockutil_tls_free(SSL *ssl)
{
    if (!ssl) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

int sockutil_tls_send(SSL *ssl, const void *buf, int len)
{
    if (!ssl || len <= 0) return -1;
    return SSL_write(ssl, buf, len);
}

int sockutil_tls_recv(SSL *ssl, void *buf, int len)
{
    if (!ssl || len <= 0) return -1;
    return SSL_read(ssl, buf, len);
}

bool sockutil_send_all(sockutil_socket_t fd, SSL *ssl,
                       const void *buf, usize len)
{
    const char *p    = (const char *)buf;
    usize       left = len;
    while (left > 0) {
        int chunk = (int)(left > 65536 ? 65536 : left);
        int n = ssl ? sockutil_tls_send(ssl, p, chunk)
                    : sockutil_send_raw(fd, p, chunk);
        if (n <= 0) return false;
        p    += n;
        left -= (usize)n;
    }
    return true;
}
