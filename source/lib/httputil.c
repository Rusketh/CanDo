/*
 * lib/httputil.c -- Shared HTTP/HTTPS utilities for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "httputil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#if defined(CANDO_PLATFORM_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSESOCK(fd) closesocket(fd)
#  define SOCK_ERRNO() WSAGetLastError()
typedef int socklen_t_compat;
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <sys/time.h>
#  define CLOSESOCK(fd) close(fd)
#  define SOCK_ERRNO() errno
#endif

/* =========================================================================
 * Dynamic byte buffer
 * ===================================================================== */

void httpbuf_init(HttpBuf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void httpbuf_reserve(HttpBuf *b, usize need)
{
    /* Reserve room for `need` more bytes plus a trailing NUL. */
    usize required = b->len + need + 1;
    if (required <= b->cap) return;

    usize ncap = b->cap ? b->cap : 256;
    while (ncap < required) ncap *= 2;
    char *nd = (char *)realloc(b->data, ncap);
    if (!nd) { abort(); }  /* OOM; CanDo otherwise aborts in cando_alloc */
    b->data = nd;
    b->cap  = ncap;
}

void httpbuf_append(HttpBuf *b, const void *data, usize len)
{
    if (len == 0) return;
    httpbuf_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void httpbuf_append_cstr(HttpBuf *b, const char *str)
{
    if (!str) return;
    httpbuf_append(b, str, strlen(str));
}

void httpbuf_append_fmt(HttpBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need <= 0) { va_end(ap2); return; }
    httpbuf_reserve(b, (usize)need);
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (usize)need;
    b->data[b->len] = '\0';
}

void httpbuf_free(HttpBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* =========================================================================
 * URL parsing
 * ===================================================================== */

bool http_parse_url(const char *url, HttpUrl *out)
{
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *p = url;
    const char *scheme_end = strstr(p, "://");
    if (!scheme_end) return false;

    usize scheme_len = (usize)(scheme_end - p);
    if (scheme_len == 0 || scheme_len >= sizeof(out->scheme)) return false;
    memcpy(out->scheme, p, scheme_len);
    out->scheme[scheme_len] = '\0';

    /* lowercase scheme */
    for (usize i = 0; i < scheme_len; i++)
        out->scheme[i] = (char)tolower((unsigned char)out->scheme[i]);

    bool is_https = (strcmp(out->scheme, "https") == 0);
    if (!is_https && strcmp(out->scheme, "http") != 0) return false;

    p = scheme_end + 3;

    /* host (and optional :port) ends at '/' '?' or end-of-string */
    const char *host_start = p;
    const char *host_end   = p;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;

    /* split off port if present */
    const char *colon = NULL;
    for (const char *q = host_start; q < host_end; q++) {
        if (*q == ':') { colon = q; break; }
    }

    const char *hend = colon ? colon : host_end;
    usize hlen = (usize)(hend - host_start);
    if (hlen == 0 || hlen >= sizeof(out->host)) return false;
    memcpy(out->host, host_start, hlen);
    out->host[hlen] = '\0';

    if (colon) {
        char portbuf[8];
        usize plen = (usize)(host_end - colon - 1);
        if (plen == 0 || plen >= sizeof(portbuf)) return false;
        memcpy(portbuf, colon + 1, plen);
        portbuf[plen] = '\0';
        long port = strtol(portbuf, NULL, 10);
        if (port <= 0 || port > 65535) return false;
        out->port = (int)port;
    } else {
        out->port = is_https ? 443 : 80;
    }

    /* path (+ query) */
    if (*host_end == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        /* Strip fragment (#...) from path. */
        const char *path_end = host_end;
        while (*path_end && *path_end != '#') path_end++;
        usize plen = (usize)(path_end - host_end);
        if (plen >= sizeof(out->path)) return false;
        memcpy(out->path, host_end, plen);
        out->path[plen] = '\0';
    }
    return true;
}

/* =========================================================================
 * Header list
 * ===================================================================== */

static char *str_dup_lower(const char *src, usize len)
{
    char *r = (char *)malloc(len + 1);
    if (!r) abort();
    for (usize i = 0; i < len; i++)
        r[i] = (char)tolower((unsigned char)src[i]);
    r[len] = '\0';
    return r;
}

static char *str_dup_len(const char *src, usize len)
{
    char *r = (char *)malloc(len + 1);
    if (!r) abort();
    memcpy(r, src, len);
    r[len] = '\0';
    return r;
}

void http_headers_init(HttpHeaders *h)
{
    h->entries = NULL;
    h->count   = 0;
    h->cap     = 0;
}

void http_headers_add(HttpHeaders *h, const char *name, usize nlen,
                      const char *value, usize vlen)
{
    if (h->count == h->cap) {
        u32 ncap = h->cap ? h->cap * 2 : 8;
        h->entries = (HttpHeader *)realloc(h->entries, ncap * sizeof(HttpHeader));
        if (!h->entries) abort();
        h->cap = ncap;
    }
    h->entries[h->count].name  = str_dup_lower(name, nlen);
    h->entries[h->count].value = str_dup_len(value, vlen);
    h->count++;
}

const char *http_headers_get(const HttpHeaders *h, const char *name)
{
    usize nlen = strlen(name);
    char  lname[128];
    if (nlen >= sizeof(lname)) return NULL;
    for (usize i = 0; i < nlen; i++)
        lname[i] = (char)tolower((unsigned char)name[i]);
    lname[nlen] = '\0';

    for (u32 i = 0; i < h->count; i++) {
        if (strcmp(h->entries[i].name, lname) == 0)
            return h->entries[i].value;
    }
    return NULL;
}

void http_headers_free(HttpHeaders *h)
{
    for (u32 i = 0; i < h->count; i++) {
        free(h->entries[i].name);
        free(h->entries[i].value);
    }
    free(h->entries);
    h->entries = NULL;
    h->count   = 0;
    h->cap     = 0;
}

/* =========================================================================
 * One-time global init
 * ===================================================================== */

static _Atomic(int) g_init_done = 0;

void http_one_time_init(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_init_done, &expected, 1))
        return;

#if defined(CANDO_PLATFORM_WINDOWS)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* OpenSSL 1.1+ auto-initialises; this call is a no-op but harmless. */
    (void)OPENSSL_init_ssl(
        OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}

/* =========================================================================
 * Connection abstraction
 * ===================================================================== */

void http_conn_init(HttpConn *c)
{
    c->fd      = -1;
    c->is_tls  = false;
    c->ssl     = NULL;
    c->ssl_ctx = NULL;
}

static void set_timeouts(int fd, int timeout_ms)
{
    if (timeout_ms <= 0) return;
#if defined(CANDO_PLATFORM_WINDOWS)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool http_conn_connect(HttpConn *c, const HttpUrl *url, int timeout_ms)
{
    http_one_time_init();
    http_conn_init(c);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", url->port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(url->host, portstr, &hints, &res);
    if (rc != 0 || !res) return false;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_timeouts(fd, timeout_ms);
        if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        CLOSESOCK(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    c->fd = fd;
    return true;
}

bool http_conn_start_tls_client(HttpConn *c, const char *sni_host)
{
    if (c->fd < 0) return false;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return false;

    /* Use system default verify paths; we accept unverified by default
     * (callers can strengthen later).  For self-signed testing we skip verify. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        return false;
    }

    if (sni_host && *sni_host) {
        SSL_set_tlsext_host_name(ssl, sni_host);
    }

    SSL_set_fd(ssl, c->fd);

    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return false;
    }

    c->ssl     = ssl;
    c->ssl_ctx = ctx;  /* owned by this connection */
    c->is_tls  = true;
    return true;
}

bool http_conn_start_tls_server(HttpConn *c, SSL_CTX *ctx)
{
    if (c->fd < 0 || !ctx) return false;

    SSL *ssl = SSL_new(ctx);
    if (!ssl) return false;

    SSL_set_fd(ssl, c->fd);
    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        return false;
    }

    c->ssl     = ssl;
    c->ssl_ctx = NULL;  /* context is owned externally (by the server) */
    c->is_tls  = true;
    return true;
}

int http_conn_read(HttpConn *c, void *buf, int len)
{
    if (c->fd < 0) return -1;
    if (c->is_tls) {
        return SSL_read(c->ssl, buf, len);
    }
#if defined(CANDO_PLATFORM_WINDOWS)
    return recv(c->fd, (char *)buf, len, 0);
#else
    return (int)recv(c->fd, buf, (size_t)len, 0);
#endif
}

int http_conn_write(HttpConn *c, const void *buf, int len)
{
    if (c->fd < 0) return -1;
    if (c->is_tls) {
        return SSL_write(c->ssl, buf, len);
    }
#if defined(CANDO_PLATFORM_WINDOWS)
    return send(c->fd, (const char *)buf, len, 0);
#else
    return (int)send(c->fd, buf, (size_t)len, 0);
#endif
}

bool http_conn_write_all(HttpConn *c, const void *buf, usize len)
{
    const char *p = (const char *)buf;
    usize left = len;
    while (left > 0) {
        int n = http_conn_write(c, p, (int)(left > 65536 ? 65536 : left));
        if (n <= 0) return false;
        p    += n;
        left -= (usize)n;
    }
    return true;
}

void http_conn_close(HttpConn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ssl_ctx) {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }
    if (c->fd >= 0) {
        CLOSESOCK(c->fd);
        c->fd = -1;
    }
    c->is_tls = false;
}

/* =========================================================================
 * Read a line terminated by CRLF into out (NUL-terminated, CRLF stripped).
 * Returns number of bytes in the line (excluding CRLF), or -1 on error.
 * `max` is the maximum number of bytes including the NUL.
 * ===================================================================== */
static int read_crlf_line(HttpConn *conn, HttpBuf *recv_buf, usize *offset,
                          char *out, usize max)
{
    usize pos = 0;
    while (1) {
        /* Look for CRLF in recv_buf starting at *offset. */
        while (*offset < recv_buf->len) {
            if (*offset + 1 < recv_buf->len &&
                recv_buf->data[*offset]     == '\r' &&
                recv_buf->data[*offset + 1] == '\n') {
                if (pos >= max) return -1;
                out[pos] = '\0';
                *offset += 2;
                return (int)pos;
            }
            if (pos + 1 >= max) return -1;
            out[pos++] = recv_buf->data[*offset];
            (*offset)++;
        }
        /* Refill buffer. */
        char tmp[1024];
        int n = http_conn_read(conn, tmp, sizeof(tmp));
        if (n <= 0) return -1;
        httpbuf_append(recv_buf, tmp, (usize)n);
    }
}

/* =========================================================================
 * Parse headers into h, reading lines until an empty line.
 * Returns true on success.
 * ===================================================================== */
static bool parse_headers(HttpConn *conn, HttpBuf *recv_buf, usize *offset,
                          HttpHeaders *h)
{
    char line[8192];
    while (1) {
        int n = read_crlf_line(conn, recv_buf, offset, line, sizeof(line));
        if (n < 0) return false;
        if (n == 0) return true;  /* empty line: end of headers */

        char *colon = strchr(line, ':');
        if (!colon) continue;  /* malformed; skip */
        usize nlen = (usize)(colon - line);
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        usize vlen = strlen(v);
        /* trim trailing whitespace */
        while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
            vlen--;
        http_headers_add(h, line, nlen, v, vlen);
    }
}

/* Read exactly `need` bytes from recv_buf (spilling from conn) into out. */
static bool read_body_bytes(HttpConn *conn, HttpBuf *recv_buf, usize *offset,
                            HttpBuf *out, usize need)
{
    /* First, consume whatever is already buffered. */
    usize avail = recv_buf->len - *offset;
    if (avail > 0) {
        usize take = avail < need ? avail : need;
        httpbuf_append(out, recv_buf->data + *offset, take);
        *offset += take;
        need    -= take;
    }
    while (need > 0) {
        char tmp[8192];
        int  want = (int)(need > sizeof(tmp) ? sizeof(tmp) : need);
        int  n    = http_conn_read(conn, tmp, want);
        if (n <= 0) return false;
        httpbuf_append(out, tmp, (usize)n);
        need -= (usize)n;
    }
    return true;
}

/* Read chunked body (RFC 7230 s4.1). */
static bool read_chunked_body(HttpConn *conn, HttpBuf *recv_buf,
                              usize *offset, HttpBuf *out)
{
    char line[64];
    while (1) {
        int n = read_crlf_line(conn, recv_buf, offset, line, sizeof(line));
        if (n < 0) return false;
        /* chunk size is hex; optionally followed by ';extensions' */
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        unsigned long sz = strtoul(line, NULL, 16);
        if (sz == 0) {
            /* trailer headers until empty line */
            while (1) {
                n = read_crlf_line(conn, recv_buf, offset, line, sizeof(line));
                if (n < 0) return false;
                if (n == 0) break;
            }
            return true;
        }
        if (!read_body_bytes(conn, recv_buf, offset, out, sz)) return false;
        /* trailing CRLF after each chunk */
        n = read_crlf_line(conn, recv_buf, offset, line, sizeof(line));
        if (n != 0) return false;
    }
}

/* =========================================================================
 * HttpParsedRequest (server)
 * ===================================================================== */

void http_parsed_request_init(HttpParsedRequest *r)
{
    r->method[0]  = '\0';
    r->path[0]    = '\0';
    r->version[0] = '\0';
    http_headers_init(&r->headers);
    httpbuf_init(&r->body);
}

void http_parsed_request_free(HttpParsedRequest *r)
{
    http_headers_free(&r->headers);
    httpbuf_free(&r->body);
}

bool http_read_request(HttpConn *conn, HttpParsedRequest *r)
{
    HttpBuf recv;
    httpbuf_init(&recv);
    usize offset = 0;

    char line[8192];
    int n = read_crlf_line(conn, &recv, &offset, line, sizeof(line));
    if (n <= 0) { httpbuf_free(&recv); return false; }

    /* METHOD SP TARGET SP HTTP/X.Y */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { httpbuf_free(&recv); return false; }
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) { httpbuf_free(&recv); return false; }
    *sp2 = '\0';
    char *ver = sp2 + 1;

    usize ml = strlen(line);
    if (ml >= sizeof(r->method)) ml = sizeof(r->method) - 1;
    memcpy(r->method, line, ml); r->method[ml] = '\0';

    usize tl = strlen(target);
    if (tl >= sizeof(r->path)) tl = sizeof(r->path) - 1;
    memcpy(r->path, target, tl); r->path[tl] = '\0';

    /* version is HTTP/<major>.<minor> */
    if (strncmp(ver, "HTTP/", 5) == 0) ver += 5;
    usize vl = strlen(ver);
    if (vl >= sizeof(r->version)) vl = sizeof(r->version) - 1;
    memcpy(r->version, ver, vl); r->version[vl] = '\0';

    if (!parse_headers(conn, &recv, &offset, &r->headers)) {
        httpbuf_free(&recv);
        return false;
    }

    /* Body (server side): we only support Content-Length.
     * Most clients use this for POST/PUT/PATCH requests. */
    const char *te = http_headers_get(&r->headers, "transfer-encoding");
    if (te && strcasecmp(te, "chunked") == 0) {
        if (!read_chunked_body(conn, &recv, &offset, &r->body)) {
            httpbuf_free(&recv);
            return false;
        }
    } else {
        const char *cl = http_headers_get(&r->headers, "content-length");
        if (cl) {
            unsigned long len = strtoul(cl, NULL, 10);
            if (len > 0 && !read_body_bytes(conn, &recv, &offset, &r->body, len)) {
                httpbuf_free(&recv);
                return false;
            }
        }
    }
    httpbuf_free(&recv);
    return true;
}

/* =========================================================================
 * HttpResponse (client)
 * ===================================================================== */

void http_response_init(HttpResponse *r)
{
    r->status    = 0;
    r->reason[0] = '\0';
    http_headers_init(&r->headers);
    httpbuf_init(&r->body);
}

void http_response_free(HttpResponse *r)
{
    http_headers_free(&r->headers);
    httpbuf_free(&r->body);
}

bool http_read_response(HttpConn *conn, HttpResponse *r)
{
    HttpBuf recv;
    httpbuf_init(&recv);
    usize offset = 0;

    char line[8192];
    int n = read_crlf_line(conn, &recv, &offset, line, sizeof(line));
    if (n <= 0) { httpbuf_free(&recv); return false; }

    /* HTTP/X.Y SP STATUS SP REASON */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { httpbuf_free(&recv); return false; }
    *sp1 = '\0';
    char *code_s = sp1 + 1;
    char *sp2 = strchr(code_s, ' ');
    if (sp2) { *sp2 = '\0'; }

    r->status = (int)strtol(code_s, NULL, 10);
    if (sp2) {
        const char *reason = sp2 + 1;
        usize rl = strlen(reason);
        if (rl >= sizeof(r->reason)) rl = sizeof(r->reason) - 1;
        memcpy(r->reason, reason, rl); r->reason[rl] = '\0';
    }

    if (!parse_headers(conn, &recv, &offset, &r->headers)) {
        httpbuf_free(&recv);
        return false;
    }

    const char *te = http_headers_get(&r->headers, "transfer-encoding");
    const char *cl = http_headers_get(&r->headers, "content-length");
    const char *cn = http_headers_get(&r->headers, "connection");
    bool close_delim = (cn && strcasecmp(cn, "close") == 0);

    if (te && strcasecmp(te, "chunked") == 0) {
        if (!read_chunked_body(conn, &recv, &offset, &r->body)) {
            httpbuf_free(&recv);
            return false;
        }
    } else if (cl) {
        unsigned long len = strtoul(cl, NULL, 10);
        if (len > 0 && !read_body_bytes(conn, &recv, &offset, &r->body, len)) {
            httpbuf_free(&recv);
            return false;
        }
    } else if (close_delim || r->status == 200 || r->status == 204) {
        /* Read until EOF. */
        /* consume any remaining bytes in recv first */
        if (offset < recv.len) {
            httpbuf_append(&r->body, recv.data + offset, recv.len - offset);
            offset = recv.len;
        }
        char tmp[8192];
        int got;
        while ((got = http_conn_read(conn, tmp, sizeof(tmp))) > 0) {
            httpbuf_append(&r->body, tmp, (usize)got);
        }
    }
    httpbuf_free(&recv);
    return true;
}

/* =========================================================================
 * Status code to reason phrase mapping
 * ===================================================================== */

const char *http_status_text(int code)
{
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a teapot";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "";
    }
}
