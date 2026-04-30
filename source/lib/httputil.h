/*
 * lib/httputil.h -- Shared HTTP utilities used by http.c and https.c.
 *
 * Provides:
 *   HttpBuf           -- dynamic byte buffer (auto-growing)
 *   HttpUrl           -- parsed URL (scheme, host, port, path)
 *   HttpHeaders       -- ordered list of HTTP headers (lowercased names)
 *   HttpConn          -- socket/TLS abstraction for read/write/close
 *   HttpParsedRequest -- parsed incoming HTTP request (server side)
 *   HttpResponse      -- parsed HTTP response (client side)
 *
 * TLS support is always enabled; OpenSSL is a build-time dependency.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_HTTPUTIL_H
#define CANDO_LIB_HTTPUTIL_H

#include "../core/common.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

/* =========================================================================
 * Dynamic byte buffer
 * ===================================================================== */
typedef struct {
    char  *data;   /* heap-allocated, NUL-terminated after each append */
    usize  len;
    usize  cap;
} HttpBuf;

void httpbuf_init(HttpBuf *b);
void httpbuf_reserve(HttpBuf *b, usize need);
void httpbuf_append(HttpBuf *b, const void *data, usize len);
void httpbuf_append_cstr(HttpBuf *b, const char *str);
void httpbuf_append_fmt(HttpBuf *b, const char *fmt, ...);
void httpbuf_free(HttpBuf *b);

/* =========================================================================
 * URL parsing
 * ===================================================================== */
typedef struct {
    char scheme[8];      /* "http" or "https"                             */
    char host[256];      /* hostname or IP                                */
    int  port;           /* explicit port, or default (80/443)            */
    char path[2048];     /* path + query string, always starts with '/'   */
} HttpUrl;

/*
 * http_parse_url -- parse url into out.  Returns true on success.
 * Accepts "http://host[:port][/path?query]" and "https://..." forms.
 */
bool http_parse_url(const char *url, HttpUrl *out);

/* =========================================================================
 * Header list
 * ===================================================================== */
typedef struct {
    char *name;    /* lowercased, heap-allocated NUL-terminated */
    char *value;   /* heap-allocated NUL-terminated             */
} HttpHeader;

typedef struct {
    HttpHeader *entries;
    u32         count;
    u32         cap;
} HttpHeaders;

void        http_headers_init(HttpHeaders *h);
void        http_headers_add(HttpHeaders *h, const char *name, usize nlen,
                             const char *value, usize vlen);
const char *http_headers_get(const HttpHeaders *h, const char *name);
void        http_headers_free(HttpHeaders *h);

/* =========================================================================
 * Connection abstraction (plain TCP or TLS)
 * ===================================================================== */
typedef struct {
    int       fd;         /* -1 when closed */
    bool      is_tls;
    SSL      *ssl;        /* valid iff is_tls */
    SSL_CTX  *ssl_ctx;    /* owned iff this connection created it (client) */
} HttpConn;

void http_conn_init(HttpConn *c);

/*
 * http_conn_connect -- resolve host, open TCP socket, connect.
 * url->port specifies the port.  Returns true on success.
 */
bool http_conn_connect(HttpConn *c, const HttpUrl *url, int timeout_ms);

/*
 * http_conn_start_tls_client -- wrap an already-connected socket with a TLS
 * client session.  Allocates a new SSL_CTX.  Returns true on success.
 * If sni_host is non-NULL it is used for SNI / verify.
 */
bool http_conn_start_tls_client(HttpConn *c, const char *sni_host);

/*
 * http_conn_start_tls_server -- wrap an already-accepted socket with a TLS
 * server session using an externally-owned SSL_CTX (from the server).
 * Returns true on success.
 */
bool http_conn_start_tls_server(HttpConn *c, SSL_CTX *ctx);

int  http_conn_read(HttpConn *c, void *buf, int len);
int  http_conn_write(HttpConn *c, const void *buf, int len);
bool http_conn_write_all(HttpConn *c, const void *buf, usize len);
void http_conn_close(HttpConn *c);

/* =========================================================================
 * Parsed request (server receives this from a client)
 * ===================================================================== */
typedef struct {
    char         method[16];
    char         path[2048];   /* request-target as received              */
    char         version[8];   /* "1.0" or "1.1"                          */
    HttpHeaders  headers;
    HttpBuf      body;
} HttpParsedRequest;

void http_parsed_request_init(HttpParsedRequest *r);
void http_parsed_request_free(HttpParsedRequest *r);

/*
 * http_read_request -- read and parse a full HTTP request from conn into r.
 * Honours Content-Length; chunked decoding on the server side is NOT
 * supported (returns 400 if requested) to keep the implementation compact.
 * Returns true on success.
 */
bool http_read_request(HttpConn *conn, HttpParsedRequest *r);

/* =========================================================================
 * HTTP response parsing (client receives this from a server)
 * ===================================================================== */
typedef struct {
    int          status;
    char         reason[64];
    HttpHeaders  headers;
    HttpBuf      body;
} HttpResponse;

void http_response_init(HttpResponse *r);
void http_response_free(HttpResponse *r);

/*
 * http_read_response -- read and parse an HTTP response from conn into r.
 * Handles Content-Length and Transfer-Encoding: chunked.  Returns true on
 * success.
 */
bool http_read_response(HttpConn *conn, HttpResponse *r);

/* =========================================================================
 * Streaming response parser + body reader
 *
 * For callers that want to drain the body lazily (e.g. pipe straight to a
 * file without buffering the whole download).  Workflow:
 *
 *     HttpResponseHead head;
 *     http_response_head_init(&head);
 *     http_read_response_head(conn, &head);   // reads only status + headers
 *     HttpBodyReader br;
 *     http_body_reader_init(&br, &head, conn);
 *     for (;;) {
 *         u8 buf[4096];
 *         int n = http_body_reader_read(&br, buf, sizeof(buf));
 *         if (n <= 0) break;
 *         consume(buf, n);
 *     }
 *     http_body_reader_free(&br);
 *     http_response_head_free(&head);
 *
 * The reader supports Content-Length, Transfer-Encoding: chunked, and
 * connection-close framing — same coverage as http_read_response.
 * ===================================================================== */
typedef enum {
    HBF_NONE    = 0,
    HBF_LENGTH  = 1,    /* Content-Length: N                  */
    HBF_CHUNKED = 2,    /* Transfer-Encoding: chunked         */
    HBF_CLOSE   = 3,    /* Read until peer closes (HTTP/1.0)  */
} HttpBodyFraming;

typedef struct {
    int             status;
    char            reason[64];
    HttpHeaders     headers;
    HttpBodyFraming framing;
    usize           length_remaining; /* meaningful only if HBF_LENGTH */
    HttpBuf         tail;             /* over-read body bytes */
    usize           tail_off;
} HttpResponseHead;

void http_response_head_init(HttpResponseHead *r);
void http_response_head_free(HttpResponseHead *r);

/*
 * http_read_response_head -- read status line + headers; do NOT touch the
 * body.  Any over-read bytes are stashed in r->tail for the body reader.
 * Returns true on success.
 */
bool http_read_response_head(HttpConn *conn, HttpResponseHead *r);

typedef struct {
    HttpConn       *conn;            /* borrowed; not closed by reader */
    HttpBodyFraming framing;
    usize           length_remaining;
    int             chunk_state;     /* 0=size_line 1=in_chunk 2=trail_crlf 3=done */
    usize           chunk_remaining;
    HttpBuf         recv;            /* internal buffer (seeded from head.tail) */
    usize           recv_off;
    bool            eof;
} HttpBodyReader;

void http_body_reader_init(HttpBodyReader *br,
                           const HttpResponseHead *head, HttpConn *conn);
void http_body_reader_free(HttpBodyReader *br);

/*
 * http_body_reader_read -- pull up to `cap` bytes into `out`.  Returns:
 *   >0   bytes read
 *    0   clean EOF (body fully consumed); br->eof is now true
 *   -1   I/O or framing error
 */
int  http_body_reader_read(HttpBodyReader *br, void *out, usize cap);

/* =========================================================================
 * Miscellaneous helpers
 * ===================================================================== */

/* Return HTTP/1.1 reason phrase for a status code (static storage). */
const char *http_status_text(int code);

/*
 * http_one_time_init -- idempotent global init (WSAStartup on Windows,
 * OpenSSL init on all platforms).  Safe to call repeatedly.
 */
void http_one_time_init(void);

#endif /* CANDO_LIB_HTTPUTIL_H */
