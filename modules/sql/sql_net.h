/*
 * modules/sql/sql_net.h -- Synchronous TCP / TLS read+write helpers
 * shared by both drivers.
 *
 * Both PostgreSQL and MySQL frame their messages with a fixed-size
 * length prefix.  The drivers ask for "exactly N bytes" or "send these
 * N bytes" -- this header centralises the loop that handles partial
 * reads/writes, the optional OpenSSL detour, and timeout signalling.
 *
 * Header-only so the module can include it from sql_pg.c / sql_mysql.c
 * without a separate translation unit.  Each driver has an SqlNet
 * embedded inside its connection struct.
 */

#ifndef CANDO_SQL_NET_H
#define CANDO_SQL_NET_H

#include "lib/sockutil.h"

#include <openssl/ssl.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct SqlNet {
    sockutil_socket_t fd;
    SSL              *ssl;        /* NULL for plain TCP */
    SSL_CTX          *ctx;        /* owned; freed in sql_net_close */
    int               io_timeout_ms;
} SqlNet;

static inline void sql_net_init(SqlNet *n)
{
    n->fd  = SOCKUTIL_INVALID_SOCKET;
    n->ssl = NULL;
    n->ctx = NULL;
    n->io_timeout_ms = 0;
}

static inline void sql_net_close(SqlNet *n)
{
    if (n->ssl) {
        sockutil_tls_free(n->ssl);
        n->ssl = NULL;
    }
    if (n->fd != SOCKUTIL_INVALID_SOCKET) {
        sockutil_close(n->fd);
        n->fd = SOCKUTIL_INVALID_SOCKET;
    }
    if (n->ctx) {
        SSL_CTX_free(n->ctx);
        n->ctx = NULL;
    }
}

/* sql_net_send_all -- write `len` bytes; loops until done.  Returns
 * true on success, false on socket error. */
static inline bool sql_net_send_all(SqlNet *n, const void *buf, size_t len)
{
    return sockutil_send_all(n->fd, n->ssl, buf, len);
}

/* sql_net_recv_exact -- read exactly `len` bytes.  Returns true on
 * success, false on EOF / socket error / timeout. */
static inline bool sql_net_recv_exact(SqlNet *n, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < len) {
        int chunk = (int)((len - got) > 0x7fffffff ? 0x7fffffff : (len - got));
        int rc;
        if (n->ssl) rc = sockutil_tls_recv(n->ssl, p + got, chunk);
        else        rc = sockutil_recv_raw(n->fd, p + got, chunk);
        if (rc <= 0) return false;
        got += (size_t)rc;
    }
    return true;
}

#endif /* CANDO_SQL_NET_H */
