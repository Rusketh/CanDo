/*
 * lib/http.c -- HTTP/HTTPS standard library for Cando.
 *
 * Implements:
 *   - http.request(opts)              synchronous HTTP client
 *   - http.get(url)                   convenience GET
 *   - http.createServer(callback)     HTTP server (background accept thread)
 *   - fetch(url [, opts])             global convenience (auto http/https)
 *
 * Server-scoped response methods (registered on each res object):
 *   res.status(code), res.setHeader(name, val), res.send(body), res.json(v)
 *
 * See httputil.c/h for the underlying HTTP parsing and connection layer.
 *
 * Must compile with gcc -std=c11.
 */

#include "http.h"
#include "httputil.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/array.h"
#include "../object/string.h"
#include "../core/thread_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <ctype.h>
#include <stdint.h>

#if defined(CANDO_PLATFORM_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define HTTP_CLOSE(fd) closesocket(fd)
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  define HTTP_CLOSE(fd) close(fd)
#endif

/* =========================================================================
 * Global server and response pools
 *
 * We hand out integer indices into these pools via the __server_id and
 * __res_id fields on the CanDo object.  This avoids casting C pointers to
 * doubles and provides bounded resource use.
 * ===================================================================== */

#define HTTP_MAX_SERVERS           32
#define HTTP_MAX_ACTIVE_RESPONSES  512

typedef struct HttpServer {
    int              listen_fd;
    _Atomic(bool)    running;
    cando_thread_t   accept_thread;
    bool             has_thread;
    CandoVM         *parent_vm;
    CandoValue       callback_fn;
    _Atomic(u32)     active_conns;
    SSL_CTX         *ssl_ctx;        /* non-NULL for HTTPS; owned here */
    bool             in_use;
    u16              port;
} HttpServer;

typedef struct HttpResCtx {
    HttpConn     conn;
    int          status_code;
    HttpHeaders  headers;
    bool         sent;
    bool         active;
} HttpResCtx;

static HttpServer    g_servers[HTTP_MAX_SERVERS];
static HttpResCtx    g_res_pool[HTTP_MAX_ACTIVE_RESPONSES];
static cando_mutex_t g_server_mutex;
static cando_mutex_t g_res_pool_mutex;
static _Atomic(int)  g_pools_inited = 0;

static void ensure_pools_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_pools_inited, &expected, 1)) {
        cando_os_mutex_init(&g_server_mutex);
        cando_os_mutex_init(&g_res_pool_mutex);
        for (int i = 0; i < HTTP_MAX_SERVERS; i++) g_servers[i].in_use = false;
        for (int i = 0; i < HTTP_MAX_ACTIVE_RESPONSES; i++) g_res_pool[i].active = false;
    }
}

static int server_alloc(void)
{
    cando_os_mutex_lock(&g_server_mutex);
    int idx = -1;
    for (int i = 0; i < HTTP_MAX_SERVERS; i++) {
        if (!g_servers[i].in_use) {
            memset(&g_servers[i], 0, sizeof(HttpServer));
            g_servers[i].in_use  = true;
            g_servers[i].listen_fd = -1;
            atomic_store(&g_servers[i].running, false);
            atomic_store(&g_servers[i].active_conns, 0);
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_server_mutex);
    return idx;
}

static void server_free(int idx)
{
    if (idx < 0 || idx >= HTTP_MAX_SERVERS) return;
    cando_os_mutex_lock(&g_server_mutex);
    g_servers[idx].in_use = false;
    cando_os_mutex_unlock(&g_server_mutex);
}

static int res_alloc(void)
{
    cando_os_mutex_lock(&g_res_pool_mutex);
    int idx = -1;
    for (int i = 0; i < HTTP_MAX_ACTIVE_RESPONSES; i++) {
        if (!g_res_pool[i].active) {
            memset(&g_res_pool[i], 0, sizeof(HttpResCtx));
            http_conn_init(&g_res_pool[i].conn);
            http_headers_init(&g_res_pool[i].headers);
            g_res_pool[i].active = true;
            g_res_pool[i].status_code = 200;
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_res_pool_mutex);
    return idx;
}

static void res_free(int idx)
{
    if (idx < 0 || idx >= HTTP_MAX_ACTIVE_RESPONSES) return;
    cando_os_mutex_lock(&g_res_pool_mutex);
    http_headers_free(&g_res_pool[idx].headers);
    g_res_pool[idx].active = false;
    cando_os_mutex_unlock(&g_res_pool_mutex);
}

/* =========================================================================
 * Helpers: obtain integer field value from object.
 * ===================================================================== */

static bool get_int_field(CdoObject *obj, const char *name, int *out)
{
    /* rawget borrows — do not release the returned value. */
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue  v    = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = (int)v.as.number;
    return true;
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

static void set_num_field(CdoObject *obj, const char *name, f64 n)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(n), FIELD_NONE);
    cdo_string_release(key);
}

static void set_bool_field(CdoObject *obj, const char *name, bool b)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_bool(b), FIELD_NONE);
    cdo_string_release(key);
}

static void set_obj_field(CdoObject *obj, const char *name, CdoObject *child)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_object_value(child), FIELD_NONE);
    cdo_string_release(key);
}

/* Iterate headers from a CdoObject (user-supplied {"Name": "Value", ...}). */
typedef struct { HttpBuf *buf; } HeaderAppendUD;

static bool append_header_iter(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)flags;
    HeaderAppendUD *a = (HeaderAppendUD *)ud;
    if (val->tag != CDO_STRING) return true;
    httpbuf_append(a->buf, key->data, key->length);
    httpbuf_append_cstr(a->buf, ": ");
    httpbuf_append(a->buf, val->as.string->data, val->as.string->length);
    httpbuf_append_cstr(a->buf, "\r\n");
    return true;
}

/* Case-insensitive header-name lookup on a user-supplied headers object.
 * Returns true if any key matches `name` (case-insensitive). */
typedef struct { const char *lname; u32 nlen; bool found; } HasHeaderUD;

static bool has_header_iter(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)val; (void)flags;
    HasHeaderUD *u = (HasHeaderUD *)ud;
    if (key->length != u->nlen) return true;
    for (u32 i = 0; i < u->nlen; i++) {
        char a = (char)tolower((unsigned char)key->data[i]);
        if (a != u->lname[i]) return true;
    }
    u->found = true;
    return false;  /* stop iteration */
}

static bool has_header_name(CdoObject *headers_obj, const char *name)
{
    if (!headers_obj) return false;
    char lname[128];
    u32 nlen = (u32)strlen(name);
    if (nlen >= sizeof(lname)) return false;
    for (u32 i = 0; i < nlen; i++)
        lname[i] = (char)tolower((unsigned char)name[i]);
    lname[nlen] = '\0';
    HasHeaderUD u = { lname, nlen, false };
    cdo_object_foreach(headers_obj, has_header_iter, &u);
    return u.found;
}

/* Look up a string field on an options object.  Returns NULL (and sets *len
 * to 0) if missing or non-string.  The returned pointer is owned by the
 * CdoString and remains valid while the object holds the field. */
static const char *get_str_field(CdoObject *obj, const char *name, u32 *len)
{
    /* rawget borrows — do not release the returned value.  The underlying
     * CdoString stays alive for as long as obj holds the slot. */
    *len = 0;
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue  v    = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_STRING) return NULL;
    const char *data = v.as.string->data;
    *len = v.as.string->length;
    return data;
}

static CdoObject *get_obj_field(CdoObject *obj, const char *name)
{
    /* rawget borrows — do not release. */
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue  v    = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok) return NULL;
    if (v.tag != CDO_OBJECT && v.tag != CDO_ARRAY) return NULL;
    return v.as.object;
}

/* =========================================================================
 * Shared client workhorse: http_do_request_native
 *
 * Accepts either a bare URL string or an options object with fields
 *   { url, method, headers, body, timeout }.
 * is_tls_hint forces TLS regardless of scheme (used by https.request).
 * Pushes a single result object { status, ok, body, headers } on success.
 * ===================================================================== */
int http_do_request_native(CandoVM *vm, int argc, CandoValue *args,
                           bool is_tls_hint)
{
    ensure_pools_inited();
    http_one_time_init();

    const char *url_cstr = NULL;
    u32         url_len  = 0;
    const char *method   = "GET";
    const char *body     = NULL;
    u32         body_len = 0;
    int         timeout  = 30000;
    CdoObject  *headers_obj = NULL;

    /* Allow either a URL string or an options object as args[0]. */
    if (argc < 1) {
        cando_vm_error(vm, "http.request: expected url string or options object");
        return -1;
    }

    CdoObject *opts = NULL;
    if (cando_is_string(args[0])) {
        url_cstr = args[0].as.string->data;
        url_len  = args[0].as.string->length;
        /* fetch(url, opts) form: options may be in args[1]. */
        if (argc >= 2 && cando_is_object(args[1])) {
            opts = cando_bridge_resolve(vm, args[1].as.handle);
        }
    } else if (cando_is_object(args[0])) {
        opts = cando_bridge_resolve(vm, args[0].as.handle);
    } else {
        cando_vm_error(vm, "http.request: expected url string or options object");
        return -1;
    }

    if (opts) {
        u32 n = 0;
        if (!url_cstr) {
            url_cstr = get_str_field(opts, "url", &n);
            url_len  = n;
        }
        const char *m = get_str_field(opts, "method", &n);
        if (m && n > 0) method = m;
        body = get_str_field(opts, "body", &body_len);
        headers_obj = get_obj_field(opts, "headers");
        int t = 0;
        if (get_int_field(opts, "timeout", &t) && t > 0) timeout = t;
    }

    if (!url_cstr || url_len == 0) {
        cando_vm_error(vm, "http.request: missing 'url'");
        return -1;
    }

    /* Copy to NUL-terminated local buffer for the parser. */
    char urlbuf[2600];
    if (url_len >= sizeof(urlbuf)) {
        cando_vm_error(vm, "http.request: url too long");
        return -1;
    }
    memcpy(urlbuf, url_cstr, url_len);
    urlbuf[url_len] = '\0';

    HttpUrl url;
    if (!http_parse_url(urlbuf, &url)) {
        cando_vm_error(vm, "http.request: invalid url");
        return -1;
    }

    bool use_tls = is_tls_hint || (strcmp(url.scheme, "https") == 0);

    /* Open connection. */
    HttpConn conn;
    if (!http_conn_connect(&conn, &url, timeout)) {
        cando_vm_error(vm, "http.request: connect failed: %s:%d",
                       url.host, url.port);
        return -1;
    }
    if (use_tls) {
        if (!http_conn_start_tls_client(&conn, url.host)) {
            http_conn_close(&conn);
            cando_vm_error(vm, "http.request: TLS handshake failed");
            return -1;
        }
    }

    /* Build request. */
    HttpBuf req;
    httpbuf_init(&req);
    httpbuf_append_fmt(&req, "%s %s HTTP/1.1\r\n", method, url.path);

    /* Host header (include port only if non-default). */
    bool have_host = headers_obj && has_header_name(headers_obj, "host");
    if (!have_host) {
        int default_port = use_tls ? 443 : 80;
        if (url.port == default_port)
            httpbuf_append_fmt(&req, "Host: %s\r\n", url.host);
        else
            httpbuf_append_fmt(&req, "Host: %s:%d\r\n", url.host, url.port);
    }
    if (!headers_obj || !has_header_name(headers_obj, "user-agent"))
        httpbuf_append_cstr(&req, "User-Agent: CanDo/1.0\r\n");
    if (!headers_obj || !has_header_name(headers_obj, "accept"))
        httpbuf_append_cstr(&req, "Accept: */*\r\n");
    if (!headers_obj || !has_header_name(headers_obj, "connection"))
        httpbuf_append_cstr(&req, "Connection: close\r\n");
    if (body_len > 0 &&
        (!headers_obj || !has_header_name(headers_obj, "content-length"))) {
        httpbuf_append_fmt(&req, "Content-Length: %u\r\n", body_len);
    }

    if (headers_obj) {
        HeaderAppendUD ud = { &req };
        cdo_object_foreach(headers_obj, append_header_iter, &ud);
    }
    httpbuf_append_cstr(&req, "\r\n");
    if (body_len > 0) httpbuf_append(&req, body, body_len);

    /* Send. */
    if (!http_conn_write_all(&conn, req.data, req.len)) {
        httpbuf_free(&req);
        http_conn_close(&conn);
        cando_vm_error(vm, "http.request: send failed");
        return -1;
    }
    httpbuf_free(&req);

    /* Read response. */
    HttpResponse resp;
    http_response_init(&resp);
    if (!http_read_response(&conn, &resp)) {
        http_response_free(&resp);
        http_conn_close(&conn);
        cando_vm_error(vm, "http.request: bad or truncated response");
        return -1;
    }
    http_conn_close(&conn);

    /* Build result object { status, ok, body, headers }. */
    CandoValue result_val = cando_bridge_new_object(vm);
    CdoObject *result     = cando_bridge_resolve(vm, result_val.as.handle);

    set_num_field(result, "status", (f64)resp.status);
    set_bool_field(result, "ok", resp.status >= 200 && resp.status < 300);
    set_str_field(result, "body",
                  resp.body.data ? resp.body.data : "",
                  (u32)resp.body.len);

    /* headers: plain object with lowercased keys. */
    CandoValue hdrs_val = cando_bridge_new_object(vm);
    CdoObject *hdrs     = cando_bridge_resolve(vm, hdrs_val.as.handle);
    for (u32 i = 0; i < resp.headers.count; i++) {
        set_str_field(hdrs,
                      resp.headers.entries[i].name,
                      resp.headers.entries[i].value,
                      (u32)strlen(resp.headers.entries[i].value));
    }
    set_obj_field(result, "headers", hdrs);

    http_response_free(&resp);

    cando_vm_push(vm, result_val);
    return 1;
}

/* Client-facing natives. */
static int http_request_fn(CandoVM *vm, int argc, CandoValue *args)
{
    return http_do_request_native(vm, argc, args, false);
}

static int http_get_fn(CandoVM *vm, int argc, CandoValue *args)
{
    /* http.get(url) -- shortcut; force GET method. */
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "http.get: expected url string");
        return -1;
    }
    /* Wrap into a tiny options object with method=GET and reuse the workhorse. */
    CandoValue opts_val = cando_bridge_new_object(vm);
    CdoObject *opts     = cando_bridge_resolve(vm, opts_val.as.handle);
    set_str_field(opts, "url", args[0].as.string->data, args[0].as.string->length);
    set_str_field(opts, "method", "GET", 3);

    CandoValue single[1] = { opts_val };
    return http_do_request_native(vm, 1, single, false);
}

static int fetch_fn(CandoVM *vm, int argc, CandoValue *args)
{
    /* fetch(url [, opts]) -- scheme decides http vs https. */
    return http_do_request_native(vm, argc, args, false);
}

/* =========================================================================
 * Response-object helpers
 * ===================================================================== */

static HttpResCtx *res_get_ctx_from(CandoVM *vm, CandoValue receiver)
{
    if (!cando_is_object(receiver)) return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, receiver.as.handle);
    if (!obj) return NULL;
    int idx = -1;
    if (!get_int_field(obj, "__res_id", &idx)) return NULL;
    if (idx < 0 || idx >= HTTP_MAX_ACTIVE_RESPONSES) return NULL;
    if (!g_res_pool[idx].active) return NULL;
    return &g_res_pool[idx];
}

/* Build and send a complete response on ctx->conn. */
static bool res_send_impl(HttpResCtx *ctx, const char *body, usize body_len)
{
    HttpBuf out;
    httpbuf_init(&out);

    const char *reason = http_status_text(ctx->status_code);
    httpbuf_append_fmt(&out, "HTTP/1.1 %d %s\r\n",
                      ctx->status_code,
                      reason && *reason ? reason : "OK");

    /* Auto-default Content-Type if the user didn't set one. */
    bool has_ct = false, has_cl = false, has_cn = false;
    for (u32 i = 0; i < ctx->headers.count; i++) {
        if (strcmp(ctx->headers.entries[i].name, "content-type") == 0) has_ct = true;
        if (strcmp(ctx->headers.entries[i].name, "content-length") == 0) has_cl = true;
        if (strcmp(ctx->headers.entries[i].name, "connection") == 0) has_cn = true;
    }
    if (!has_ct) httpbuf_append_cstr(&out, "Content-Type: text/html; charset=utf-8\r\n");
    if (!has_cn) httpbuf_append_cstr(&out, "Connection: close\r\n");

    for (u32 i = 0; i < ctx->headers.count; i++) {
        httpbuf_append_cstr(&out, ctx->headers.entries[i].name);
        httpbuf_append_cstr(&out, ": ");
        httpbuf_append_cstr(&out, ctx->headers.entries[i].value);
        httpbuf_append_cstr(&out, "\r\n");
    }
    if (!has_cl) httpbuf_append_fmt(&out, "Content-Length: %zu\r\n", body_len);
    httpbuf_append_cstr(&out, "\r\n");
    if (body_len > 0 && body) httpbuf_append(&out, body, body_len);

    bool ok = http_conn_write_all(&ctx->conn, out.data, out.len);
    httpbuf_free(&out);
    ctx->sent = true;
    return ok;
}

/* res.status(code) */
static int res_status_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpResCtx *ctx = res_get_ctx_from(vm, args[0]);
    if (!ctx) { cando_vm_push(vm, args[0]); return 1; }
    int code = (int)libutil_arg_num_at(args, argc, 1, 200);
    ctx->status_code = code;
    cando_vm_push(vm, args[0]);  /* return receiver for chaining */
    return 1;
}

/* res.setHeader(name, value) */
static int res_setHeader_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpResCtx *ctx = res_get_ctx_from(vm, args[0]);
    const char *name  = libutil_arg_cstr_at(args, argc, 1);
    const char *value = libutil_arg_cstr_at(args, argc, 2);
    if (ctx && name && value) {
        http_headers_add(&ctx->headers,
                         name,  strlen(name),
                         value, strlen(value));
    }
    cando_vm_push(vm, args[0]);
    return 1;
}

/* res.send(body) */
static int res_send_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpResCtx *ctx = res_get_ctx_from(vm, args[0]);
    if (!ctx || ctx->sent) { cando_vm_push(vm, args[0]); return 1; }

    const char *body = "";
    u32         blen = 0;
    char       *tmp  = NULL;
    if (argc >= 2) {
        if (cando_is_string(args[1])) {
            body = args[1].as.string->data;
            blen = args[1].as.string->length;
        } else if (!cando_is_null(args[1])) {
            tmp = cando_value_tostring(args[1]);
            if (tmp) { body = tmp; blen = (u32)strlen(tmp); }
        }
    }
    res_send_impl(ctx, body, blen);
    if (tmp) free(tmp);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* res.json(value) — uses the `json` global's stringify method. */
static int res_json_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpResCtx *ctx = res_get_ctx_from(vm, args[0]);
    if (!ctx || ctx->sent) { cando_vm_push(vm, args[0]); return 1; }

    /* Look up the json global and its stringify method. */
    CandoValue json_val = cando_null();
    if (!cando_vm_get_global(vm, "json", &json_val) || !cando_is_object(json_val)) {
        cando_vm_error(vm, "res.json: json global not available");
        return -1;
    }
    CdoObject *json_obj = cando_bridge_resolve(vm, json_val.as.handle);
    CdoString *key = cdo_string_intern("stringify", 9);
    CdoValue stringify_cv = cdo_null();
    bool ok = cdo_object_rawget(json_obj, key, &stringify_cv);
    cdo_string_release(key);
    if (!ok) {
        cando_vm_error(vm, "res.json: json.stringify not found");
        return -1;
    }
    /* stringify is a native sentinel (negative number); wrap in CandoValue. */
    CandoValue stringify_fn = cando_number(stringify_cv.as.number);
    cdo_value_release(stringify_cv);

    CandoValue call_args[1] = { argc >= 2 ? args[1] : cando_null() };
    int nret = cando_vm_call_value(vm, stringify_fn, call_args, 1);
    if (vm->has_error) return -1;
    if (nret <= 0) {
        cando_vm_error(vm, "res.json: stringify returned no value");
        return -1;
    }
    CandoValue jstr = cando_vm_pop(vm);

    /* Add Content-Type if not present. */
    bool has_ct = false;
    for (u32 i = 0; i < ctx->headers.count; i++) {
        if (strcmp(ctx->headers.entries[i].name, "content-type") == 0) { has_ct = true; break; }
    }
    if (!has_ct) {
        http_headers_add(&ctx->headers,
                         "content-type", 12,
                         "application/json; charset=utf-8", 31);
    }
    if (cando_is_string(jstr)) {
        res_send_impl(ctx, jstr.as.string->data, jstr.as.string->length);
    } else {
        res_send_impl(ctx, "null", 4);
    }
    cando_value_release(jstr);
    cando_vm_push(vm, args[0]);
    return 1;
}

/* =========================================================================
 * Server accept + connection threads
 * ===================================================================== */

typedef struct ConnArg {
    int server_idx;
    int client_fd;
} ConnArg;

static CANDO_THREAD_RETURN conn_thread_fn(void *arg_p)
{
    ConnArg *arg = (ConnArg *)arg_p;
    int sidx = arg->server_idx;
    int cfd  = arg->client_fd;
    free(arg);

    if (sidx < 0 || sidx >= HTTP_MAX_SERVERS || !g_servers[sidx].in_use) {
        HTTP_CLOSE(cfd);
        return CANDO_THREAD_RETURN_VAL;
    }
    HttpServer *server = &g_servers[sidx];
    atomic_fetch_add(&server->active_conns, 1);

    int res_idx = res_alloc();
    if (res_idx < 0) {
        HTTP_CLOSE(cfd);
        atomic_fetch_sub(&server->active_conns, 1);
        return CANDO_THREAD_RETURN_VAL;
    }
    HttpResCtx *ctx = &g_res_pool[res_idx];
    ctx->conn.fd = cfd;

    /* TLS wrap if HTTPS server. */
    if (server->ssl_ctx) {
        if (!http_conn_start_tls_server(&ctx->conn, server->ssl_ctx)) {
            res_free(res_idx);
            HTTP_CLOSE(cfd);
            atomic_fetch_sub(&server->active_conns, 1);
            return CANDO_THREAD_RETURN_VAL;
        }
    }

    /* Parse the request. */
    HttpParsedRequest preq;
    http_parsed_request_init(&preq);
    if (!http_read_request(&ctx->conn, &preq)) {
        /* Send a terse 400 then cleanup. */
        const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        http_conn_write_all(&ctx->conn, bad, sizeof(bad) - 1);
        http_parsed_request_free(&preq);
        http_conn_close(&ctx->conn);
        res_free(res_idx);
        atomic_fetch_sub(&server->active_conns, 1);
        return CANDO_THREAD_RETURN_VAL;
    }

    /* Spin up a child VM sharing globals/handles with the parent. */
    CandoVM child;
    cando_vm_init_child(&child, server->parent_vm);

    /* Build req object. */
    CandoValue req_val = cando_bridge_new_object(&child);
    CdoObject *req_obj = cando_bridge_resolve(&child, req_val.as.handle);
    set_str_field(req_obj, "method",  preq.method,  (u32)strlen(preq.method));
    set_str_field(req_obj, "url",     preq.path,    (u32)strlen(preq.path));
    /* Split path and query at '?'. */
    const char *qmark = strchr(preq.path, '?');
    if (qmark) {
        set_str_field(req_obj, "path",  preq.path, (u32)(qmark - preq.path));
        set_str_field(req_obj, "query", qmark + 1, (u32)strlen(qmark + 1));
    } else {
        set_str_field(req_obj, "path",  preq.path, (u32)strlen(preq.path));
        set_str_field(req_obj, "query", "", 0);
    }
    set_str_field(req_obj, "httpVersion", preq.version, (u32)strlen(preq.version));
    set_str_field(req_obj, "body",
                  preq.body.data ? preq.body.data : "",
                  (u32)preq.body.len);

    CandoValue hdrs_val = cando_bridge_new_object(&child);
    CdoObject *hdrs_obj = cando_bridge_resolve(&child, hdrs_val.as.handle);
    for (u32 i = 0; i < preq.headers.count; i++) {
        set_str_field(hdrs_obj,
                      preq.headers.entries[i].name,
                      preq.headers.entries[i].value,
                      (u32)strlen(preq.headers.entries[i].value));
    }
    set_obj_field(req_obj, "headers", hdrs_obj);

    /* Build res object. */
    CandoValue res_val = cando_bridge_new_object(&child);
    CdoObject *res_obj = cando_bridge_resolve(&child, res_val.as.handle);
    set_num_field(res_obj, "__res_id", (f64)res_idx);
    libutil_set_method(&child, res_obj, "status",    res_status_fn);
    libutil_set_method(&child, res_obj, "setHeader", res_setHeader_fn);
    libutil_set_method(&child, res_obj, "send",      res_send_fn);
    libutil_set_method(&child, res_obj, "json",      res_json_fn);

    /* Call the user callback(req, res). */
    CandoValue cb_args[2] = { req_val, res_val };
    cando_vm_call_value(&child, server->callback_fn, cb_args, 2);

    /* If the callback did not send anything, flush a default 500. */
    if (!ctx->sent) {
        ctx->status_code = 500;
        res_send_impl(ctx, "Internal Server Error", 21);
    }

    /* Cleanup. */
    http_parsed_request_free(&preq);
    http_conn_close(&ctx->conn);
    res_free(res_idx);
    cando_vm_destroy(&child);
    atomic_fetch_sub(&server->active_conns, 1);
    return CANDO_THREAD_RETURN_VAL;
}

static CANDO_THREAD_RETURN accept_thread_fn(void *arg)
{
    int sidx = (int)(intptr_t)arg;
    if (sidx < 0 || sidx >= HTTP_MAX_SERVERS) return CANDO_THREAD_RETURN_VAL;
    HttpServer *server = &g_servers[sidx];

    while (atomic_load(&server->running)) {
        struct sockaddr_storage sa;
        socklen_t slen = sizeof(sa);
        int cfd = (int)accept(server->listen_fd, (struct sockaddr *)&sa, &slen);
        if (cfd < 0) {
            /* Timeout or interrupted — re-check the running flag. */
            continue;
        }
        ConnArg *carg = (ConnArg *)malloc(sizeof(ConnArg));
        if (!carg) { HTTP_CLOSE(cfd); continue; }
        carg->server_idx = sidx;
        carg->client_fd  = cfd;

        cando_thread_t t;
        if (!cando_os_thread_create(&t, conn_thread_fn, carg)) {
            HTTP_CLOSE(cfd);
            free(carg);
            continue;
        }
        cando_os_thread_detach(t);
    }
    return CANDO_THREAD_RETURN_VAL;
}

/* =========================================================================
 * Server methods: server.listen(port [, host])  server.close()
 * ===================================================================== */

static HttpServer *server_get_from(CandoVM *vm, CandoValue receiver)
{
    if (!cando_is_object(receiver)) return NULL;
    CdoObject *obj = cando_bridge_resolve(vm, receiver.as.handle);
    if (!obj) return NULL;
    int idx = -1;
    if (!get_int_field(obj, "__server_id", &idx)) return NULL;
    if (idx < 0 || idx >= HTTP_MAX_SERVERS) return NULL;
    if (!g_servers[idx].in_use) return NULL;
    return &g_servers[idx];
}

static int server_listen_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpServer *server = server_get_from(vm, args[0]);
    if (!server) {
        cando_vm_error(vm, "server.listen: invalid server receiver");
        return -1;
    }
    int port = (int)libutil_arg_num_at(args, argc, 1, 0);
    if (port <= 0 || port > 65535) {
        cando_vm_error(vm, "server.listen: invalid port");
        return -1;
    }
    const char *host = libutil_arg_cstr_at(args, argc, 2);
    if (!host) host = "0.0.0.0";

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        cando_vm_error(vm, "server.listen: socket() failed");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u16)port);
    if (strcmp(host, "0.0.0.0") == 0) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
            HTTP_CLOSE(fd);
            cando_vm_error(vm, "server.listen: bad host '%s'", host);
            return -1;
        }
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        HTTP_CLOSE(fd);
        cando_vm_error(vm, "server.listen: bind() failed on port %d", port);
        return -1;
    }
    if (listen(fd, 64) < 0) {
        HTTP_CLOSE(fd);
        cando_vm_error(vm, "server.listen: listen() failed");
        return -1;
    }
    /* Short accept timeout so accept_thread_fn can re-poll server->running. */
#if defined(CANDO_PLATFORM_WINDOWS)
    DWORD tv = 500;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv = { 0, 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    server->listen_fd = fd;
    server->port      = (u16)port;
    atomic_store(&server->running, true);

    int sidx = (int)(server - g_servers);
    if (!cando_os_thread_create(&server->accept_thread, accept_thread_fn,
                                (void *)(intptr_t)sidx)) {
        atomic_store(&server->running, false);
        HTTP_CLOSE(fd);
        server->listen_fd = -1;
        cando_vm_error(vm, "server.listen: failed to start accept thread");
        return -1;
    }
    server->has_thread = true;

    cando_vm_push(vm, args[0]);  /* return receiver */
    return 1;
}

static int server_close_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { cando_vm_push(vm, cando_null()); return 1; }
    HttpServer *server = server_get_from(vm, args[0]);
    if (!server) { cando_vm_push(vm, cando_null()); return 1; }

    atomic_store(&server->running, false);
    if (server->listen_fd >= 0) {
#if defined(CANDO_PLATFORM_WINDOWS)
        shutdown(server->listen_fd, SD_BOTH);
#else
        shutdown(server->listen_fd, SHUT_RDWR);
#endif
        HTTP_CLOSE(server->listen_fd);
        server->listen_fd = -1;
    }
    if (server->has_thread) {
        cando_os_thread_join(server->accept_thread);
        server->has_thread = false;
    }
    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
    }
    cando_value_release(server->callback_fn);
    server->callback_fn = cando_null();

    int sidx = (int)(server - g_servers);
    server_free(sidx);

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * http.createServer / https.createServer implementation
 * ===================================================================== */

int http_create_server_native_impl(CandoVM *vm, CandoValue callback,
                                   struct SSL_CTX_st *ssl_ctx)
{
    ensure_pools_inited();
    http_one_time_init();

    if (!cando_is_object(callback)) {
        cando_vm_error(vm, "createServer: callback must be a function");
        return -1;
    }

    int sidx = server_alloc();
    if (sidx < 0) {
        cando_vm_error(vm, "createServer: too many active servers");
        return -1;
    }
    HttpServer *server = &g_servers[sidx];
    server->parent_vm   = vm;
    server->callback_fn = cando_value_copy(callback);  /* retain */
    server->ssl_ctx     = (SSL_CTX *)ssl_ctx;
    server->listen_fd   = -1;

    CandoValue sobj_val = cando_bridge_new_object(vm);
    CdoObject *sobj     = cando_bridge_resolve(vm, sobj_val.as.handle);
    set_num_field(sobj, "__server_id", (f64)sidx);
    libutil_set_method(vm, sobj, "listen", server_listen_fn);
    libutil_set_method(vm, sobj, "close",  server_close_fn);

    cando_vm_push(vm, sobj_val);
    return 1;
}

static int http_create_server_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "http.createServer: expected callback");
        return -1;
    }
    return http_create_server_native_impl(vm, args[0], NULL);
}

/* =========================================================================
 * Registration
 * ===================================================================== */

void cando_lib_http_register(CandoVM *vm)
{
    ensure_pools_inited();
    http_one_time_init();

    CandoValue http_val = cando_bridge_new_object(vm);
    CdoObject *http_obj = cando_bridge_resolve(vm, http_val.as.handle);

    libutil_set_method(vm, http_obj, "request",      http_request_fn);
    libutil_set_method(vm, http_obj, "get",          http_get_fn);
    libutil_set_method(vm, http_obj, "createServer", http_create_server_fn);

    cando_vm_set_global(vm, "http", http_val, true);

    /* Register global fetch(). */
    cando_vm_register_native(vm, "fetch", fetch_fn);
}

