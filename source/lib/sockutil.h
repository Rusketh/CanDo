/*
 * lib/sockutil.h -- Cross-platform TCP and TLS primitives shared by the
 *                   `socket`, `secure_socket`, `http`, and `https` libraries.
 *
 * The goal of this module is to centralise the messy parts of network code
 * (winsock vs POSIX, getaddrinfo dual-stack resolution, OpenSSL context
 * setup, TLS handshake, SO_RCVTIMEO/SO_SNDTIMEO, address-to-string) so that
 * higher-level libraries can express their logic without re-implementing the
 * same scaffolding in every file.
 *
 * Threading: every function is reentrant.  sockutil_one_time_init() uses an
 * atomic compare-exchange to ensure WSAStartup() and OpenSSL global init
 * happen exactly once per process regardless of how many threads call it.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_SOCKUTIL_H
#define CANDO_LIB_SOCKUTIL_H

#include "../core/common.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#if defined(CANDO_PLATFORM_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#endif

/* =========================================================================
 * Cross-platform socket type
 *
 * On POSIX a socket is a small `int` file descriptor; on Windows it is an
 * unsigned `SOCKET`.  We unify on `int` because every CanDo native wraps the
 * value in a CanDo number (f64) and -1 is a convenient invalid sentinel.
 * Windows SOCKET values do fit comfortably in 31 bits in practice.
 * ===================================================================== */
typedef int sockutil_socket_t;
#define SOCKUTIL_INVALID_SOCKET (-1)

/* =========================================================================
 * One-time global initialisation
 *
 * Performs WSAStartup() on Windows, ignores SIGPIPE on POSIX (so a broken
 * connection returns -1/EPIPE instead of killing the process), and runs the
 * OpenSSL string-loading bootstrap.  Idempotent and thread-safe.
 * ===================================================================== */
CANDO_API void sockutil_one_time_init(void);

/* =========================================================================
 * Address family selection
 *
 * Use these in the `family` argument of sockutil_tcp_connect / _listen.
 * AF_UNSPEC means "let getaddrinfo pick" — the call walks the result list
 * trying each address.
 * ===================================================================== */

/* =========================================================================
 * TCP client / server
 * ===================================================================== */

/*
 * sockutil_tcp_connect -- resolve `host:port` (dual-stack via getaddrinfo)
 * and open a connected TCP socket.  `family` may be AF_INET, AF_INET6, or
 * AF_UNSPEC.  `timeout_ms` <= 0 means no SO_*TIMEO is configured.
 *
 * Returns the connected socket on success or SOCKUTIL_INVALID_SOCKET on
 * failure; `err` is populated with a short reason string when non-NULL.
 */
CANDO_API sockutil_socket_t sockutil_tcp_connect(const char *host, int port,
                                                 int family, int timeout_ms,
                                                 char *err, usize errlen);

/*
 * sockutil_tcp_listen -- create a TCP listener bound to `host:port`.
 *
 * `host` may be NULL or "" for any-interface bind.  `family` may be AF_INET,
 * AF_INET6, or AF_UNSPEC; on AF_UNSPEC we try IPv6 first (with
 * IPV6_V6ONLY=0 so it accepts IPv4-mapped clients too) and fall back to IPv4
 * if IPv6 is not configured on the host.  `backlog` <= 0 maps to 64.
 *
 * The returned socket has SO_REUSEADDR set.
 *
 * Returns SOCKUTIL_INVALID_SOCKET on failure.
 */
CANDO_API sockutil_socket_t sockutil_tcp_listen(const char *host, int port,
                                                int family, int backlog,
                                                char *err, usize errlen);

/*
 * sockutil_tcp_accept -- accept a single connection from `listen_fd`.
 *
 * If `timeout_ms > 0` the call uses SO_RCVTIMEO so the wait can be bounded;
 * a timeout returns SOCKUTIL_INVALID_SOCKET with err = "timeout".
 *
 * `peer_addr`/`peer_len` are populated with the client address when non-NULL.
 */
CANDO_API sockutil_socket_t sockutil_tcp_accept(sockutil_socket_t listen_fd,
                                                int timeout_ms,
                                                struct sockaddr_storage *peer_addr,
                                                socklen_t *peer_len,
                                                char *err, usize errlen);

/* =========================================================================
 * Socket lifecycle and options
 * ===================================================================== */

CANDO_API void sockutil_close(sockutil_socket_t fd);
CANDO_API void sockutil_shutdown(sockutil_socket_t fd);

/* timeout_ms <= 0 disables timeouts (resets to blocking). */
CANDO_API void sockutil_set_timeout(sockutil_socket_t fd, int timeout_ms);

CANDO_API bool sockutil_set_blocking  (sockutil_socket_t fd, bool blocking);
CANDO_API bool sockutil_set_reuseaddr (sockutil_socket_t fd, bool yes);
CANDO_API bool sockutil_set_nodelay   (sockutil_socket_t fd, bool yes);
CANDO_API bool sockutil_set_keepalive (sockutil_socket_t fd, bool yes);
CANDO_API bool sockutil_set_recvbuf   (sockutil_socket_t fd, int bytes);
CANDO_API bool sockutil_set_sendbuf   (sockutil_socket_t fd, int bytes);

/* =========================================================================
 * Address introspection
 * ===================================================================== */

CANDO_API bool sockutil_get_local_addr(sockutil_socket_t fd,
                                       struct sockaddr_storage *out,
                                       socklen_t *outlen);
CANDO_API bool sockutil_get_peer_addr (sockutil_socket_t fd,
                                       struct sockaddr_storage *out,
                                       socklen_t *outlen);

/*
 * sockutil_addr_to_string -- decompose a sockaddr_storage into host/port/family.
 *
 * `host_out` receives a numeric textual address (no DNS).  `family_out` is
 * set to AF_INET or AF_INET6.  Returns true on success.
 */
CANDO_API bool sockutil_addr_to_string(const struct sockaddr_storage *sa,
                                       socklen_t len,
                                       char *host_out, usize host_max,
                                       int *port_out, int *family_out);

/*
 * sockutil_resolve -- run getaddrinfo for `host` (port irrelevant) and write
 * up to `max_out` numeric address strings into `out`.  Returns the number
 * written.  Use this to implement script-level `socket.resolve()`.
 */
CANDO_API int sockutil_resolve(const char *host, int family,
                               char (*out)[INET6_ADDRSTRLEN], int max_out);

/* =========================================================================
 * Raw byte I/O on a plain TCP socket
 *
 * These wrap recv()/send() with the WSAGetLastError vs errno difference and
 * the SOCKET vs int signedness on Windows.  Return value semantics mirror
 * recv/send: > 0 = bytes transferred, 0 = clean EOF (recv only), -1 = error.
 * ===================================================================== */

CANDO_API int sockutil_send_raw(sockutil_socket_t fd,
                                const void *buf, int len);
CANDO_API int sockutil_recv_raw(sockutil_socket_t fd,
                                void *buf, int len);

/* =========================================================================
 * TLS
 * ===================================================================== */

/*
 * Client-side TLS context options.  Pass NULL for fields that should remain
 * at their OpenSSL defaults.
 *
 * verify_peer
 *     When true, sets SSL_VERIFY_PEER and loads the system trust store via
 *     SSL_CTX_set_default_verify_paths().  When false, certificate
 *     verification is disabled (useful for self-signed test environments;
 *     production code should always set this to true).
 *
 * ca_pem / ca_pem_len
 *     Optional additional CA bundle in PEM form.  Adds the certs to the
 *     trust store on top of (or instead of, when verify_peer is true and
 *     the system store is unavailable) the default paths.
 *
 * cert_pem / key_pem
 *     Optional client certificate + private key for mutual TLS.
 */
typedef struct SockutilTlsClientOpts {
    bool        verify_peer;
    const char *ca_pem;
    u32         ca_pem_len;
    const char *cert_pem;
    u32         cert_pem_len;
    const char *key_pem;
    u32         key_pem_len;
} SockutilTlsClientOpts;

/*
 * Server-side TLS context options.
 *
 * cert_pem / key_pem are required.  ca_pem (when non-NULL) is loaded as the
 * acceptable client-cert chain when `verify_peer` is true.
 */
typedef struct SockutilTlsServerOpts {
    bool        verify_peer;
    const char *ca_pem;
    u32         ca_pem_len;
} SockutilTlsServerOpts;

CANDO_API SSL_CTX *sockutil_build_client_ssl_ctx(const SockutilTlsClientOpts *opts,
                                                 char *err, usize errlen);

CANDO_API SSL_CTX *sockutil_build_server_ssl_ctx(const char *cert_pem, u32 cert_len,
                                                 const char *key_pem,  u32 key_len,
                                                 const SockutilTlsServerOpts *opts,
                                                 char *err, usize errlen);

/*
 * sockutil_tls_wrap -- attach a TLS session (client or server) to an already
 * connected TCP socket.  Performs SSL_connect or SSL_accept synchronously.
 *
 * For a client, `sni_host` (when non-NULL/non-empty) is used both for SNI and
 * for hostname verification when the underlying ctx has verify enabled.
 *
 * Returns the SSL* on success (caller frees with sockutil_tls_free), or NULL
 * on failure.
 */
CANDO_API SSL *sockutil_tls_wrap(sockutil_socket_t fd, SSL_CTX *ctx,
                                 bool is_client, const char *sni_host,
                                 char *err, usize errlen);

CANDO_API void sockutil_tls_free(SSL *ssl);

CANDO_API int  sockutil_tls_send(SSL *ssl, const void *buf, int len);
CANDO_API int  sockutil_tls_recv(SSL *ssl, void *buf, int len);

/*
 * sockutil_send_all -- write `len` bytes; loops until done or error.
 * If `ssl` is non-NULL the data is sent via TLS, otherwise via raw fd.
 */
CANDO_API bool sockutil_send_all(sockutil_socket_t fd, SSL *ssl,
                                 const void *buf, usize len);

#endif /* CANDO_LIB_SOCKUTIL_H */
