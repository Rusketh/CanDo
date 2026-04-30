/*
 * lib/httputil.c -- Shared HTTP/HTTPS utilities for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "httputil.h"
#include "sockutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

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
 * One-time global init  (delegates to sockutil)
 * ===================================================================== */

void http_one_time_init(void)
{
    sockutil_one_time_init();
}

/* =========================================================================
 * Connection abstraction
 *
 * HttpConn is now a thin wrapper around sockutil's primitives.  The fd field
 * is preserved for binary compatibility with http.c which directly assigns
 * incoming `accept`-returned descriptors into ctx->conn.fd.
 * ===================================================================== */

void http_conn_init(HttpConn *c)
{
    c->fd      = -1;
    c->is_tls  = false;
    c->ssl     = NULL;
    c->ssl_ctx = NULL;
}

bool http_conn_connect(HttpConn *c, const HttpUrl *url, int timeout_ms)
{
    http_conn_init(c);
    sockutil_socket_t fd = sockutil_tcp_connect(url->host, url->port,
                                                AF_UNSPEC, timeout_ms,
                                                NULL, 0);
    if (fd == SOCKUTIL_INVALID_SOCKET) return false;
    c->fd = (int)fd;
    return true;
}

bool http_conn_start_tls_client(HttpConn *c, const char *sni_host)
{
    if (c->fd < 0) return false;

    /* Match the previous behaviour of httputil: no peer verification by
     * default.  Callers that require verification should use the new
     * `secure_socket` library which defaults verifyPeer to true. */
    SockutilTlsClientOpts opts = { 0 };
    opts.verify_peer = false;

    SSL_CTX *ctx = sockutil_build_client_ssl_ctx(&opts, NULL, 0);
    if (!ctx) return false;

    SSL *ssl = sockutil_tls_wrap((sockutil_socket_t)c->fd, ctx, true,
                                 sni_host, NULL, 0);
    if (!ssl) {
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

    SSL *ssl = sockutil_tls_wrap((sockutil_socket_t)c->fd, ctx, false,
                                 NULL, NULL, 0);
    if (!ssl) return false;

    c->ssl     = ssl;
    c->ssl_ctx = NULL;  /* context is owned externally (by the server) */
    c->is_tls  = true;
    return true;
}

int http_conn_read(HttpConn *c, void *buf, int len)
{
    if (c->fd < 0) return -1;
    if (c->is_tls) return sockutil_tls_recv(c->ssl, buf, len);
    return sockutil_recv_raw((sockutil_socket_t)c->fd, buf, len);
}

int http_conn_write(HttpConn *c, const void *buf, int len)
{
    if (c->fd < 0) return -1;
    if (c->is_tls) return sockutil_tls_send(c->ssl, buf, len);
    return sockutil_send_raw((sockutil_socket_t)c->fd, buf, len);
}

bool http_conn_write_all(HttpConn *c, const void *buf, usize len)
{
    if (c->fd < 0) return false;
    return sockutil_send_all((sockutil_socket_t)c->fd,
                             c->is_tls ? c->ssl : NULL,
                             buf, len);
}

void http_conn_close(HttpConn *c)
{
    if (c->ssl) {
        sockutil_tls_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ssl_ctx) {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }
    if (c->fd >= 0) {
        sockutil_close((sockutil_socket_t)c->fd);
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
 * Streaming response: header-only parser + body state machine
 * ===================================================================== */

void http_response_head_init(HttpResponseHead *r)
{
    r->status     = 0;
    r->reason[0]  = '\0';
    r->framing    = HBF_NONE;
    r->length_remaining = 0;
    r->tail_off   = 0;
    http_headers_init(&r->headers);
    httpbuf_init(&r->tail);
}

void http_response_head_free(HttpResponseHead *r)
{
    http_headers_free(&r->headers);
    httpbuf_free(&r->tail);
}

bool http_read_response_head(HttpConn *conn, HttpResponseHead *r)
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
    if (sp2) *sp2 = '\0';

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

    /* Choose framing exactly as http_read_response does so the streamed
     * path matches the buffered path byte-for-byte for any given server. */
    const char *te = http_headers_get(&r->headers, "transfer-encoding");
    const char *cl = http_headers_get(&r->headers, "content-length");
    const char *cn = http_headers_get(&r->headers, "connection");
    bool close_delim = (cn && strcasecmp(cn, "close") == 0);

    if (te && strcasecmp(te, "chunked") == 0) {
        r->framing = HBF_CHUNKED;
    } else if (cl) {
        r->framing = HBF_LENGTH;
        r->length_remaining = strtoul(cl, NULL, 10);
    } else if (close_delim || r->status == 200 || r->status == 204) {
        r->framing = HBF_CLOSE;
    } else {
        r->framing = HBF_NONE;
    }

    /* Stash any bytes we over-read past the headers so the body reader
     * sees them before going back to the wire. */
    if (offset < recv.len) {
        httpbuf_append(&r->tail, recv.data + offset, recv.len - offset);
    }
    r->tail_off = 0;
    httpbuf_free(&recv);
    return true;
}

void http_body_reader_init(HttpBodyReader *br,
                           const HttpResponseHead *head, HttpConn *conn)
{
    br->conn             = conn;
    br->framing          = head->framing;
    br->length_remaining = head->length_remaining;
    br->chunk_state      = 0;
    br->chunk_remaining  = 0;
    br->recv_off         = 0;
    br->eof              = (head->framing == HBF_NONE) ||
                           (head->framing == HBF_LENGTH &&
                            head->length_remaining == 0);
    httpbuf_init(&br->recv);
    /* Seed the reader's recv buffer with whatever the head parser
     * already pulled past the header terminator. */
    if (head->tail.len > head->tail_off) {
        httpbuf_append(&br->recv,
                       head->tail.data + head->tail_off,
                       head->tail.len - head->tail_off);
    }
}

void http_body_reader_free(HttpBodyReader *br)
{
    httpbuf_free(&br->recv);
    br->conn = NULL;
}

/* Drain min(cap, available) bytes out of br->recv into `out`.  Returns
 * the number of bytes copied; advances recv_off. */
static usize body_drain_recv(HttpBodyReader *br, void *out, usize cap)
{
    usize avail = br->recv.len - br->recv_off;
    if (avail == 0) return 0;
    usize n = cap < avail ? cap : avail;
    memcpy(out, br->recv.data + br->recv_off, n);
    br->recv_off += n;
    return n;
}

int http_body_reader_read(HttpBodyReader *br, void *out, usize cap)
{
    if (br->eof || cap == 0) return 0;
    u8 *outb = (u8 *)out;

    if (br->framing == HBF_LENGTH) {
        usize want = cap < br->length_remaining ? cap : br->length_remaining;
        if (want == 0) { br->eof = true; return 0; }
        usize taken = body_drain_recv(br, outb, want);
        if (taken < want) {
            int n = http_conn_read(br->conn, outb + taken,
                                   (int)(want - taken));
            if (n <= 0) return -1;
            taken += (usize)n;
        }
        br->length_remaining -= taken;
        if (br->length_remaining == 0) br->eof = true;
        return (int)taken;
    }

    if (br->framing == HBF_CLOSE) {
        usize taken = body_drain_recv(br, outb, cap);
        if (taken == 0) {
            int n = http_conn_read(br->conn, outb, (int)cap);
            if (n < 0)  return -1;
            if (n == 0) { br->eof = true; return 0; }
            taken = (usize)n;
        }
        return (int)taken;
    }

    if (br->framing == HBF_CHUNKED) {
        /* Iterate the state machine until we have at least one byte for
         * the caller, hit clean EOF, or hit an error.  cap > 0 here so
         * the loop terminates as soon as `taken` advances. */
        usize taken = 0;
        while (taken == 0 && !br->eof) {
            if (br->chunk_state == 0) {
                char line[64];
                int n = read_crlf_line(br->conn, &br->recv, &br->recv_off,
                                       line, sizeof(line));
                if (n < 0) return -1;
                char *semi = strchr(line, ';');
                if (semi) *semi = '\0';
                br->chunk_remaining = (usize)strtoul(line, NULL, 16);
                if (br->chunk_remaining == 0) {
                    /* Consume any trailers, then we're done. */
                    while (1) {
                        n = read_crlf_line(br->conn, &br->recv, &br->recv_off,
                                           line, sizeof(line));
                        if (n < 0) return -1;
                        if (n == 0) break;
                    }
                    br->chunk_state = 3;
                    br->eof = true;
                    return 0;
                }
                br->chunk_state = 1;
            } else if (br->chunk_state == 1) {
                usize want = cap < br->chunk_remaining ? cap : br->chunk_remaining;
                usize from_buf = body_drain_recv(br, outb, want);
                taken += from_buf;
                br->chunk_remaining -= from_buf;
                if (br->chunk_remaining > 0 && taken < cap) {
                    usize need = cap - taken;
                    if (need > br->chunk_remaining) need = br->chunk_remaining;
                    int n = http_conn_read(br->conn, outb + taken, (int)need);
                    if (n <= 0) return -1;
                    taken += (usize)n;
                    br->chunk_remaining -= (usize)n;
                }
                if (br->chunk_remaining == 0) br->chunk_state = 2;
            } else if (br->chunk_state == 2) {
                char line[8];
                int n = read_crlf_line(br->conn, &br->recv, &br->recv_off,
                                       line, sizeof(line));
                if (n != 0) return -1;     /* expected empty CRLF */
                br->chunk_state = 0;
            }
        }
        return (int)taken;
    }

    /* HBF_NONE — no body. */
    br->eof = true;
    return 0;
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
