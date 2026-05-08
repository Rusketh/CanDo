/*
 * modules/smtp/smtp_module.c -- CanDo SMTP / IMAP / POP3 / MIME module.
 *
 * Loaded into a script with:
 *
 *     VAR mail = include("./smtp.so");        // Linux / macOS
 *     VAR mail = include("./smtp.dll");       // Windows
 *
 * Or, for embedders linking libcando directly:
 *
 *     cando_open_smtplib(vm);
 *
 * Backed by:
 *   - OpenSSL (already a hard dep of cando's secure_socket).
 *   - libresolv on POSIX, dnsapi.lib on Windows.
 *   - cando's existing sockutil layer for TCP / TLS.
 *
 * One file holds all natives.  Pure parsers / encoders live in:
 *   smtp_helpers.h, mime.h, dkim.h, spf.h, dns.h, storage.h
 * and are header-only so the C unit-test harness can link them
 * standalone without dragging in libcando.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/array.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"
#include "lib/sockutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

#include <openssl/ssl.h>

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION smtp_mutex_t;
#  define SMTP_MUTEX_INIT(m)   InitializeCriticalSection(m)
#  define SMTP_MUTEX_LOCK(m)   EnterCriticalSection(m)
#  define SMTP_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#else
#  include <pthread.h>
#  include <strings.h>
#  include <sys/stat.h>
   typedef pthread_mutex_t smtp_mutex_t;
#  define SMTP_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#  define SMTP_MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define SMTP_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#endif

#include "smtp_helpers.h"
#include "mime.h"
#include "dns.h"
#include "dkim.h"
#include "spf.h"
#include "storage.h"

/* file.read used by storage tests etc.: include the real file lib so
 * we can populate a script-side message with a body read from disk
 * without forcing the user to drag in `file` separately for the simple
 * cases.  The script can still call file.read directly when it wants
 * more control. */

/* =========================================================================
 * Connection-pool slots
 *
 * One pool is shared by SMTP / POP3 / IMAP sessions; each slot remembers
 * the protocol family so handle_unwrap can validate intent.
 * ======================================================================= */

#define SMTP_MAX_INSTANCES   256
#define SMTP_SLOT_KEY        "__smtp_slot"

typedef enum {
    SMTP_KIND_NONE = 0,
    SMTP_KIND_SMTP = 1,
    SMTP_KIND_POP3 = 2,
    SMTP_KIND_IMAP = 3,
    SMTP_KIND_SERVER = 4,
} SmtpKind;

typedef struct SmtpSlot {
    bool                 in_use;
    SmtpKind             kind;
    sockutil_socket_t    fd;
    SSL                 *ssl;        /* NULL = plain socket */
    SSL_CTX             *ctx;        /* owned */
    char                 host[256];
    int                  port;
    int                  imap_seq;   /* monotonic IMAP tag counter */
    /* Read buffer for line-based protocols. */
    linebuf_t            rbuf;
    /* Capability cache (after EHLO / CAPABILITY). */
    char                *capabilities;
} SmtpSlot;

static SmtpSlot      g_smtp_pool[SMTP_MAX_INSTANCES];
static smtp_mutex_t  g_smtp_pool_mutex;
static _Atomic(int)  g_smtp_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_smtp_pool_inited, &expected, 1)) {
        SMTP_MUTEX_INIT(&g_smtp_pool_mutex);
        for (int i = 0; i < SMTP_MAX_INSTANCES; i++) {
            g_smtp_pool[i].in_use = false;
            g_smtp_pool[i].fd     = SOCKUTIL_INVALID_SOCKET;
            g_smtp_pool[i].ssl    = NULL;
            g_smtp_pool[i].ctx    = NULL;
            g_smtp_pool[i].kind   = SMTP_KIND_NONE;
            linebuf_init(&g_smtp_pool[i].rbuf);
            g_smtp_pool[i].capabilities = NULL;
        }
        sockutil_one_time_init();
    }
}

static int pool_alloc(SmtpKind kind)
{
    ensure_pool_inited();
    SMTP_MUTEX_LOCK(&g_smtp_pool_mutex);
    int idx = -1;
    for (int i = 0; i < SMTP_MAX_INSTANCES; i++) {
        if (!g_smtp_pool[i].in_use) {
            g_smtp_pool[i].in_use = true;
            g_smtp_pool[i].kind   = kind;
            g_smtp_pool[i].fd     = SOCKUTIL_INVALID_SOCKET;
            g_smtp_pool[i].ssl    = NULL;
            g_smtp_pool[i].ctx    = NULL;
            g_smtp_pool[i].host[0] = '\0';
            g_smtp_pool[i].port    = 0;
            g_smtp_pool[i].imap_seq = 0;
            linebuf_init(&g_smtp_pool[i].rbuf);
            g_smtp_pool[i].capabilities = NULL;
            idx = i; break;
        }
    }
    SMTP_MUTEX_UNLOCK(&g_smtp_pool_mutex);
    return idx;
}

static SmtpSlot *pool_get(int idx, SmtpKind expect)
{
    if (idx < 0 || idx >= SMTP_MAX_INSTANCES) return NULL;
    if (!g_smtp_pool[idx].in_use) return NULL;
    if (expect != SMTP_KIND_NONE && g_smtp_pool[idx].kind != expect) return NULL;
    return &g_smtp_pool[idx];
}

static void pool_release(int idx)
{
    if (idx < 0 || idx >= SMTP_MAX_INSTANCES) return;
    SMTP_MUTEX_LOCK(&g_smtp_pool_mutex);
    SmtpSlot *s = &g_smtp_pool[idx];
    if (s->ssl) { sockutil_tls_free(s->ssl); s->ssl = NULL; }
    if (s->ctx) { SSL_CTX_free(s->ctx); s->ctx = NULL; }
    if (s->fd != SOCKUTIL_INVALID_SOCKET) {
        sockutil_shutdown(s->fd);
        sockutil_close(s->fd);
        s->fd = SOCKUTIL_INVALID_SOCKET;
    }
    linebuf_free(&s->rbuf);
    free(s->capabilities); s->capabilities = NULL;
    s->kind = SMTP_KIND_NONE;
    s->in_use = false;
    SMTP_MUTEX_UNLOCK(&g_smtp_pool_mutex);
}

/* =========================================================================
 * Multi-value error throws
 *
 * Scripts catch SMTP errors with `CATCH (msg, code, enhanced)`:
 *   - msg      = formatted error message
 *   - code     = numeric SMTP reply (e.g. 550, 421); 0 for module-internal
 *   - enhanced = RFC 3463 enhanced status (e.g. "5.1.1") or "" if none
 * ======================================================================= */

extern void cando_value_release(CandoValue v);

static void smtp_attach_extra(CandoVM *vm, int code, const char *enhanced)
{
    cando_value_release(vm->error_vals[1]);
    vm->error_vals[1] = cando_number((f64)code);
    cando_value_release(vm->error_vals[2]);
    const char *e = enhanced ? enhanced : "";
    CandoString *s = cando_string_new(e, (u32)strlen(e));
    vm->error_vals[2] = cando_string_value(s);
    if (vm->error_val_count < 3) vm->error_val_count = 3;
}

static void smtp_throw(CandoVM *vm, int code, const char *enhanced,
                       const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cando_vm_error(vm, "%s", buf);
    smtp_attach_extra(vm, code, enhanced);
}

/* =========================================================================
 * Script-handle helpers
 * ======================================================================= */

static bool obj_get_string(CdoObject *obj, const char *key,
                           const char **out, size_t *out_len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_STRING) return false;
    *out = v.as.string->data;
    if (out_len) *out_len = v.as.string->length;
    return true;
}

static bool obj_get_number(CdoObject *obj, const char *key, f64 *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = v.as.number;
    return true;
}

static bool obj_get_bool(CdoObject *obj, const char *key, bool *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_BOOL) return false;
    *out = v.as.boolean;
    return true;
}

static bool obj_get_object(CdoObject *obj, const char *key, CdoObject **out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok) return false;
    if (v.tag == CDO_OBJECT || v.tag == CDO_ARRAY) {
        *out = v.as.object; return true;
    }
    return false;
}

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static void obj_set_number(CdoObject *obj, const char *key, f64 v)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(v), FIELD_NONE);
    cdo_string_release(k);
}

static void obj_set_bool(CdoObject *obj, const char *key, bool v)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(v), FIELD_NONE);
    cdo_string_release(k);
}

static void obj_set_object(CdoObject *obj, const char *key, CdoObject *child)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_object_value(child), FIELD_NONE);
    cdo_string_release(k);
}

static void obj_set_array(CdoObject *obj, const char *key, CdoObject *child)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_array_value(child), FIELD_NONE);
    cdo_string_release(k);
}

/* Wrap a slot index in a fresh script object. */
static CandoValue make_handle(CandoVM *vm, int slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(obj, SMTP_SLOT_KEY, (f64)slot);
    return v;
}

static int handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    f64 idx = -1.0;
    if (!obj_get_number(obj, SMTP_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= SMTP_MAX_INSTANCES) return -1;
    return i;
}

static SmtpSlot *handle_unwrap(CandoVM *vm, CandoValue v, SmtpKind expect,
                                const char *fn)
{
    int slot = handle_slot(vm, v);
    if (slot < 0) {
        smtp_throw(vm, 0, "", "%s: expected session object", fn);
        return NULL;
    }
    SmtpSlot *s = pool_get(slot, expect);
    if (!s) {
        smtp_throw(vm, 0, "", "%s: session has been closed or wrong kind", fn);
        return NULL;
    }
    return s;
}

static void handle_mark_closed(CandoVM *vm, CandoValue v)
{
    int slot = handle_slot(vm, v);
    if (slot >= 0) pool_release(slot);
    if (cando_is_object(v)) {
        CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
        obj_set_number(obj, SMTP_SLOT_KEY, -1.0);
    }
}

/* =========================================================================
 * Wire I/O wrappers
 * ======================================================================= */

static int slot_send(SmtpSlot *s, const void *buf, int len)
{
    if (s->ssl) return sockutil_tls_send(s->ssl, buf, len);
    return sockutil_send_raw(s->fd, buf, len);
}

static bool slot_send_all(SmtpSlot *s, const void *buf, size_t len)
{
    return sockutil_send_all(s->fd, s->ssl, buf, len);
}

static int slot_recv(SmtpSlot *s, void *buf, int len)
{
    if (s->ssl) return sockutil_tls_recv(s->ssl, buf, len);
    return sockutil_recv_raw(s->fd, buf, len);
}

/* Read one CRLF-terminated line into `out`.  Returns 0 on success, -1 on
 * I/O error, -2 on line-too-long.  The CRLF is NOT included. */
static int slot_read_line(SmtpSlot *s, sb_t *out)
{
    out->len = 0;
    if (out->data) out->data[0] = '\0';
    while (1) {
        const char *p = NULL;
        long got = linebuf_take(&s->rbuf, &p);
        if (got >= 0) { sb_append(out, p, (size_t)got); return 0; }
        if (got == -2) return -2;
        char tmp[4096];
        int k = slot_recv(s, tmp, sizeof(tmp));
        if (k <= 0) return -1;
        sb_append(&s->rbuf.buf, tmp, (size_t)k);
    }
}

/* Read a multi-line SMTP reply.  An SMTP reply is a sequence of lines
 * each starting with the same 3-digit code; lines that have a '-' after
 * the code continue the reply, and a ' ' marks the final line.
 *
 * Returns the numeric code (>= 100) on success, < 0 on I/O error.  The
 * full multi-line text is appended to `text` (without the leading code
 * or hyphen, line-joined with '\n'). */
static int smtp_read_reply(SmtpSlot *s, sb_t *text)
{
    int code = -1;
    while (1) {
        sb_t line; sb_init(&line);
        int rc = slot_read_line(s, &line);
        if (rc != 0) { sb_free(&line); return -1; }
        if (line.len < 3) { sb_free(&line); return -1; }
        int c = (line.data[0] - '0') * 100 +
                (line.data[1] - '0') * 10  +
                (line.data[2] - '0');
        if (code < 0) code = c;
        bool more = line.len > 3 && line.data[3] == '-';
        const char *body = line.len > 4 ? line.data + 4 : "";
        size_t bl       = line.len > 4 ? line.len - 4 : 0;
        if (text->len) sb_putc(text, '\n');
        sb_append(text, body, bl);
        sb_free(&line);
        if (!more) break;
    }
    return code;
}

/* Send a single CRLF-terminated SMTP command. */
static bool smtp_send_line(SmtpSlot *s, const char *line)
{
    sb_t b; sb_init(&b);
    sb_puts(&b, line);
    sb_puts(&b, "\r\n");
    bool ok = slot_send_all(s, b.data, b.len);
    sb_free(&b);
    return ok;
}

/* Send a command and capture the reply. */
static int smtp_cmd(SmtpSlot *s, const char *line, sb_t *out_text)
{
    sb_t scratch; sb_init(&scratch);
    sb_t *text = out_text ? out_text : &scratch;
    if (!smtp_send_line(s, line)) {
        sb_free(&scratch);
        return -1;
    }
    int rc = smtp_read_reply(s, text);
    sb_free(&scratch);
    return rc;
}

/* =========================================================================
 * native: connect(opts) -> session
 *
 * opts:
 *   host, port, tls (bool, implicit TLS), starttls (bool, default TRUE
 *   for port 587), verifyPeer (default TRUE), timeout (seconds),
 *   auth { mech, user, password|token }, ehloName.
 * ======================================================================= */

static SSL_CTX *build_client_ctx(bool verify, char *err, size_t errlen)
{
    SockutilTlsClientOpts o = { 0 };
    o.verify_peer = verify;
    return sockutil_build_client_ssl_ctx(&o, err, errlen);
}

static int native_connect(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "smtp.connect: opts object required");
        return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);

    const char *host = NULL; size_t hostl = 0;
    f64 port = 587;
    bool tls_implicit = false, starttls_opt = true;
    bool starttls_set = false;
    bool verify       = true;
    f64  timeout      = 30;
    const char *ehlo = NULL; size_t ehlol = 0;
    obj_get_string(o, "host", &host, &hostl);
    obj_get_number(o, "port", &port);
    if (obj_get_bool(o, "tls", &tls_implicit)) { /* ok */ }
    if (obj_get_bool(o, "starttls", &starttls_opt)) starttls_set = true;
    obj_get_bool(o, "verifyPeer", &verify);
    obj_get_number(o, "timeout", &timeout);
    obj_get_string(o, "ehloName", &ehlo, &ehlol);

    if (!host) {
        smtp_throw(vm, 0, "", "smtp.connect: 'host' required");
        return -1;
    }
    /* If port is 465 default to implicit TLS. */
    if ((int)port == 465 && !obj_get_bool(o, "tls", &tls_implicit)) tls_implicit = true;
    /* If port is 25 disable starttls auto unless set. */
    if (!starttls_set && (int)port == 25) starttls_opt = false;

    int slot = pool_alloc(SMTP_KIND_SMTP);
    if (slot < 0) {
        smtp_throw(vm, 0, "", "smtp.connect: pool exhausted");
        return -1;
    }
    SmtpSlot *s = &g_smtp_pool[slot];
    snprintf(s->host, sizeof(s->host), "%.*s", (int)hostl, host);
    s->port = (int)port;

    char err[256] = {0};
    s->fd = sockutil_tcp_connect(s->host, s->port, AF_UNSPEC,
                                  (int)(timeout * 1000), err, sizeof(err));
    if (s->fd == SOCKUTIL_INVALID_SOCKET) {
        pool_release(slot);
        smtp_throw(vm, 0, "", "smtp.connect: %s:%d: %s",
                   s->host, s->port, err[0] ? err : "connect failed");
        return -1;
    }
    sockutil_set_timeout(s->fd, (int)(timeout * 1000));

    if (tls_implicit) {
        s->ctx = build_client_ctx(verify, err, sizeof(err));
        if (!s->ctx) {
            pool_release(slot);
            smtp_throw(vm, 0, "", "smtp.connect: TLS ctx: %s", err);
            return -1;
        }
        s->ssl = sockutil_tls_wrap(s->fd, s->ctx, /*is_client=*/true,
                                   s->host, err, sizeof(err));
        if (!s->ssl) {
            pool_release(slot);
            smtp_throw(vm, 0, "", "smtp.connect: TLS wrap: %s", err);
            return -1;
        }
    }

    /* Read greeting. */
    sb_t greet; sb_init(&greet);
    int gc = smtp_read_reply(s, &greet);
    if (gc != 220) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%.*s",
                 (int)(greet.len < 200 ? greet.len : 200),
                 greet.data ? greet.data : "");
        sb_free(&greet);
        pool_release(slot);
        smtp_throw(vm, gc < 0 ? 0 : gc, "",
                   "smtp.connect: greeting failed: %s", msg);
        return -1;
    }
    sb_free(&greet);

    /* EHLO. */
    char ehlo_buf[300];
    if (ehlo && ehlol)
        snprintf(ehlo_buf, sizeof(ehlo_buf), "EHLO %.*s", (int)ehlol, ehlo);
    else
        snprintf(ehlo_buf, sizeof(ehlo_buf), "EHLO localhost");
    sb_t caps; sb_init(&caps);
    int rc = smtp_cmd(s, ehlo_buf, &caps);
    if (rc != 250) {
        sb_free(&caps);
        pool_release(slot);
        smtp_throw(vm, rc < 0 ? 0 : rc, "", "smtp.connect: EHLO refused");
        return -1;
    }
    s->capabilities = strdup_n(caps.data ? caps.data : "", caps.len);

    /* STARTTLS if requested and available. */
    if (!tls_implicit && starttls_opt &&
        s->capabilities && strstr(s->capabilities, "STARTTLS")) {
        sb_t r; sb_init(&r);
        int rc2 = smtp_cmd(s, "STARTTLS", &r);
        sb_free(&r);
        if (rc2 != 220) {
            sb_free(&caps);
            pool_release(slot);
            smtp_throw(vm, rc2 < 0 ? 0 : rc2, "",
                       "smtp.connect: STARTTLS refused");
            return -1;
        }
        s->ctx = build_client_ctx(verify, err, sizeof(err));
        if (!s->ctx) {
            sb_free(&caps);
            pool_release(slot);
            smtp_throw(vm, 0, "", "smtp.connect: STARTTLS ctx: %s", err);
            return -1;
        }
        s->ssl = sockutil_tls_wrap(s->fd, s->ctx, true, s->host,
                                   err, sizeof(err));
        if (!s->ssl) {
            sb_free(&caps);
            pool_release(slot);
            smtp_throw(vm, 0, "", "smtp.connect: STARTTLS wrap: %s", err);
            return -1;
        }
        /* Re-issue EHLO after TLS. */
        sb_t caps2; sb_init(&caps2);
        rc = smtp_cmd(s, ehlo_buf, &caps2);
        if (rc != 250) {
            sb_free(&caps2); sb_free(&caps);
            pool_release(slot);
            smtp_throw(vm, rc < 0 ? 0 : rc, "",
                       "smtp.connect: post-TLS EHLO refused");
            return -1;
        }
        free(s->capabilities);
        s->capabilities = strdup_n(caps2.data ? caps2.data : "", caps2.len);
        sb_free(&caps2);
    }
    sb_free(&caps);

    /* Optional auth. */
    CdoObject *auth = NULL;
    if (obj_get_object(o, "auth", &auth)) {
        const char *mech = NULL; size_t ml = 0;
        const char *user = NULL; size_t ul = 0;
        const char *pwd  = NULL; size_t pl = 0;
        const char *tok  = NULL; size_t tl = 0;
        obj_get_string(auth, "mech",     &mech, &ml);
        obj_get_string(auth, "user",     &user, &ul);
        obj_get_string(auth, "password", &pwd,  &pl);
        obj_get_string(auth, "token",    &tok,  &tl);
        if (!mech) mech = "PLAIN";
        if (ci_eq(mech, "PLAIN")) {
            /* AUTH PLAIN \0user\0password (base64) */
            sb_t inb; sb_init(&inb);
            sb_putc(&inb, '\0');
            sb_append(&inb, user ? user : "", ul);
            sb_putc(&inb, '\0');
            sb_append(&inb, pwd ? pwd : "", pl);
            char *enc = b64_encode_alloc((const uint8_t *)inb.data, inb.len, NULL);
            sb_free(&inb);
            if (!enc) {
                pool_release(slot);
                smtp_throw(vm, 0, "", "smtp.connect: AUTH PLAIN encode failed");
                return -1;
            }
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "AUTH PLAIN %s", enc);
            free(enc);
            sb_t r; sb_init(&r);
            int rc3 = smtp_cmd(s, cmd, &r);
            if (rc3 != 235) {
                int code = rc3 < 0 ? 0 : rc3;
                pool_release(slot);
                smtp_throw(vm, code, "",
                           "smtp.connect: AUTH PLAIN failed: %.*s",
                           (int)(r.len < 200 ? r.len : 200), r.data ? r.data : "");
                sb_free(&r);
                return -1;
            }
            sb_free(&r);
        } else if (ci_eq(mech, "LOGIN")) {
            sb_t r; sb_init(&r);
            int rc3 = smtp_cmd(s, "AUTH LOGIN", &r);
            sb_free(&r);
            if (rc3 != 334) {
                pool_release(slot);
                smtp_throw(vm, rc3 < 0 ? 0 : rc3, "",
                           "smtp.connect: AUTH LOGIN refused");
                return -1;
            }
            char *u_b64 = b64_encode_alloc((const uint8_t *)(user ? user : ""),
                                           ul, NULL);
            int rc4 = smtp_cmd(s, u_b64, NULL);
            free(u_b64);
            if (rc4 != 334) {
                pool_release(slot);
                smtp_throw(vm, rc4 < 0 ? 0 : rc4, "",
                           "smtp.connect: AUTH LOGIN user rejected");
                return -1;
            }
            char *p_b64 = b64_encode_alloc((const uint8_t *)(pwd ? pwd : ""),
                                           pl, NULL);
            sb_t r2; sb_init(&r2);
            int rc5 = smtp_cmd(s, p_b64, &r2);
            free(p_b64);
            if (rc5 != 235) {
                pool_release(slot);
                smtp_throw(vm, rc5 < 0 ? 0 : rc5, "",
                           "smtp.connect: AUTH LOGIN failed: %.*s",
                           (int)(r2.len < 200 ? r2.len : 200), r2.data ? r2.data : "");
                sb_free(&r2);
                return -1;
            }
            sb_free(&r2);
        } else if (ci_eq(mech, "XOAUTH2")) {
            sb_t inb; sb_init(&inb);
            sb_putf(&inb, "user=%.*s\x01" "auth=Bearer %.*s\x01" "\x01",
                    (int)ul, user ? user : "", (int)tl, tok ? tok : "");
            char *enc = b64_encode_alloc((const uint8_t *)inb.data, inb.len, NULL);
            sb_free(&inb);
            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "AUTH XOAUTH2 %s", enc ? enc : "");
            free(enc);
            sb_t r; sb_init(&r);
            int rc3 = smtp_cmd(s, cmd, &r);
            if (rc3 != 235) {
                pool_release(slot);
                smtp_throw(vm, rc3 < 0 ? 0 : rc3, "",
                           "smtp.connect: AUTH XOAUTH2 failed: %.*s",
                           (int)(r.len < 200 ? r.len : 200), r.data ? r.data : "");
                sb_free(&r);
                return -1;
            }
            sb_free(&r);
        } else {
            pool_release(slot);
            smtp_throw(vm, 0, "", "smtp.connect: unknown auth mech '%s'", mech);
            return -1;
        }
    }

    cando_vm_push(vm, make_handle(vm, slot));
    return 1;
}

/* =========================================================================
 * native: capabilities(session) -> array<string>
 * ======================================================================= */

static int native_capabilities(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "smtp.capabilities: session required"); return -1; }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_SMTP, "smtp.capabilities");
    if (!s) return -1;
    CandoValue arr_v = cando_bridge_new_array(vm);
    CdoObject *arr   = cando_bridge_resolve(vm, arr_v.as.handle);
    if (s->capabilities) {
        const char *p = s->capabilities;
        while (*p) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            CdoString *cs = cdo_string_intern(p, (u32)(e - p));
            cdo_array_push(arr, cdo_string_value(cs));
            cdo_string_release(cs);
            p = (*e == '\n') ? e + 1 : e;
        }
    }
    cando_vm_push(vm, arr_v);
    return 1;
}

/* =========================================================================
 * native: simple SMTP verbs (mailFrom, rcptTo, data, reset, noop, quit, close)
 * ======================================================================= */

static int simple_smtp_cmd(CandoVM *vm, int argc, CandoValue *args,
                           const char *fn, const char *cmd_fmt, int success)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "%s: session required", fn); return -1; }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_SMTP, fn);
    if (!s) return -1;
    char cmd[1024];
    if (argc >= 2 && cando_is_string(args[1])) {
        const char *arg = args[1].as.string->data;
        size_t al = args[1].as.string->length;
        if (!header_value_safe(arg, al)) {
            smtp_throw(vm, 0, "", "%s: argument contains CR/LF", fn);
            return -1;
        }
        snprintf(cmd, sizeof(cmd), cmd_fmt, arg);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", cmd_fmt);
    }
    sb_t r; sb_init(&r);
    int rc = smtp_cmd(s, cmd, &r);
    if (rc != success) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%.*s",
                 (int)(r.len < 400 ? r.len : 400), r.data ? r.data : "");
        sb_free(&r);
        smtp_throw(vm, rc < 0 ? 0 : rc, "",
                   "%s: server replied %d: %s", fn, rc, buf);
        return -1;
    }
    sb_free(&r);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_mail_from(CandoVM *vm, int argc, CandoValue *args)
{
    return simple_smtp_cmd(vm, argc, args, "smtp.mailFrom",
                           "MAIL FROM:<%s>", 250);
}

static int native_rcpt_to(CandoVM *vm, int argc, CandoValue *args)
{
    return simple_smtp_cmd(vm, argc, args, "smtp.rcptTo",
                           "RCPT TO:<%s>", 250);
}

static int native_reset(CandoVM *vm, int argc, CandoValue *args)
{
    return simple_smtp_cmd(vm, argc, args, "smtp.reset", "RSET", 250);
}

static int native_noop(CandoVM *vm, int argc, CandoValue *args)
{
    return simple_smtp_cmd(vm, argc, args, "smtp.noop", "NOOP", 250);
}

/* native: data(session, raw_message) */
static int native_data(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[1])) {
        smtp_throw(vm, 0, "", "smtp.data: (session, raw_message) required");
        return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_SMTP, "smtp.data");
    if (!s) return -1;

    sb_t r1; sb_init(&r1);
    int rc = smtp_cmd(s, "DATA", &r1);
    if (rc != 354) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%.*s",
                 (int)(r1.len < 200 ? r1.len : 200), r1.data ? r1.data : "");
        sb_free(&r1);
        smtp_throw(vm, rc < 0 ? 0 : rc, "",
                   "smtp.data: DATA refused: %s", buf);
        return -1;
    }
    sb_free(&r1);

    /* Dot-stuff and write the body. */
    sb_t body; sb_init(&body);
    dot_stuff(args[1].as.string->data, args[1].as.string->length, &body);
    if (!slot_send_all(s, body.data, body.len)) {
        sb_free(&body);
        smtp_throw(vm, 0, "", "smtp.data: send failed mid-body");
        return -1;
    }
    sb_free(&body);
    if (!slot_send_all(s, ".\r\n", 3)) {
        smtp_throw(vm, 0, "", "smtp.data: send failed at terminator");
        return -1;
    }
    sb_t r2; sb_init(&r2);
    int rc2 = smtp_read_reply(s, &r2);
    if (rc2 != 250) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%.*s",
                 (int)(r2.len < 200 ? r2.len : 200), r2.data ? r2.data : "");
        sb_free(&r2);
        smtp_throw(vm, rc2 < 0 ? 0 : rc2, "",
                   "smtp.data: rejected: %s", buf);
        return -1;
    }
    sb_free(&r2);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* close = QUIT + release pool slot. */
static int native_close(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "smtp.close: session required"); return -1; }
    int slot = handle_slot(vm, args[0]);
    if (slot >= 0 && g_smtp_pool[slot].in_use &&
        g_smtp_pool[slot].kind == SMTP_KIND_SMTP) {
        sb_t r; sb_init(&r);
        smtp_cmd(&g_smtp_pool[slot], "QUIT", &r);
        sb_free(&r);
    }
    handle_mark_closed(vm, args[0]);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * High-level: send(opts) -> { messageId, accepted, rejected }
 * ======================================================================= */

/* Helper: walk a "to/cc/bcc" CdoValue (string or array) and run cb. */
typedef bool (*addr_visitor)(const char *addr_or_token,
                             size_t n, void *ud);

static void visit_address_field(CandoVM *vm, CdoObject *obj, const char *key,
                                addr_visitor cb, void *ud)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok) return;
    if (v.tag == CDO_STRING) {
        cb(v.as.string->data, v.as.string->length, ud);
    } else if (v.tag == CDO_ARRAY) {
        u32 n = cdo_array_len(v.as.object);
        for (u32 i = 0; i < n; i++) {
            CdoValue iv;
            if (cdo_array_rawget_idx(v.as.object, i, &iv) &&
                iv.tag == CDO_STRING) {
                cb(iv.as.string->data, iv.as.string->length, ud);
            }
        }
    }
}

/* Build a comma-joined string from a "to|cc" field. */
typedef struct { sb_t out; bool first; } join_ud;

static bool join_visitor(const char *addr, size_t n, void *ud)
{
    join_ud *j = (join_ud *)ud;
    if (!j->first) sb_puts(&j->out, ", ");
    sb_append(&j->out, addr, n);
    j->first = false;
    return true;
}

/* Collect bare RCPT addresses (extract the local@host out of any
 * display-name form). */
typedef struct { char (*list)[320]; size_t cap; size_t n; } rcpt_ud;

static bool rcpt_visitor(const char *raw, size_t n, void *ud)
{
    rcpt_ud *r = (rcpt_ud *)ud;
    char name[256], addr[256];
    if (!parse_one_address(raw, n, name, sizeof(name), addr, sizeof(addr)))
        return true;
    if (r->n < r->cap) {
        snprintf(r->list[r->n++], 320, "%s", addr);
    }
    return true;
}

/* The "send" call: connect, send, return result.  This mirrors mail.send
 * in the design doc.  Two modes:
 *   - server: present  -> use that smtp host
 *   - server: absent   -> direct-to-MX
 */
static int native_send(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "mail.send: opts object required");
        return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);

    const char *from = NULL; size_t froml = 0;
    if (!obj_get_string(o, "from", &from, &froml) || froml == 0) {
        smtp_throw(vm, 0, "", "mail.send: 'from' required");
        return -1;
    }

    /* Build to/cc joined strings + RCPT list. */
    join_ud jt = { .first = true }; sb_init(&jt.out);
    visit_address_field(vm, o, "to", join_visitor, &jt);
    join_ud jc = { .first = true }; sb_init(&jc.out);
    visit_address_field(vm, o, "cc", join_visitor, &jc);

    char rcpts[64][320];
    rcpt_ud ru = { .list = rcpts, .cap = 64, .n = 0 };
    visit_address_field(vm, o, "to",  rcpt_visitor, &ru);
    visit_address_field(vm, o, "cc",  rcpt_visitor, &ru);
    visit_address_field(vm, o, "bcc", rcpt_visitor, &ru);

    if (ru.n == 0) {
        sb_free(&jt.out); sb_free(&jc.out);
        smtp_throw(vm, 0, "", "mail.send: at least one recipient required");
        return -1;
    }

    /* Body: either raw or build from text/html/attachments. */
    const char *raw = NULL; size_t rawl = 0;
    obj_get_string(o, "raw", &raw, &rawl);
    sb_t built; sb_init(&built);
    const char *msg_bytes;
    size_t      msg_len;
    char       *signed_blob = NULL;

    if (raw && rawl) {
        msg_bytes = raw; msg_len = rawl;
    } else {
        const char *subject = NULL; size_t sl = 0;
        const char *text    = NULL; size_t tl = 0;
        const char *html    = NULL; size_t hl = 0;
        const char *reply_to= NULL; size_t rl = 0;
        const char *user_agent = "CanDo SMTP/1.0";
        obj_get_string(o, "subject", &subject, &sl);
        obj_get_string(o, "text",    &text,    &tl);
        obj_get_string(o, "html",    &html,    &hl);
        obj_get_string(o, "replyTo", &reply_to,&rl);

        /* Attachments. */
        mime_attach_in_t atts[16]; size_t natt = 0;
        sb_t att_bodies[16];
        for (size_t i = 0; i < 16; i++) sb_init(&att_bodies[i]);
        CdoObject *attarr = NULL;
        if (obj_get_object(o, "attachments", &attarr) && attarr) {
            u32 n = cdo_array_len(attarr);
            for (u32 i = 0; i < n && natt < 16; i++) {
                CdoValue iv;
                if (!cdo_array_rawget_idx(attarr, i, &iv)) continue;
                if (iv.tag != CDO_OBJECT) continue;
                CdoObject *ao = iv.as.object;
                const char *path = NULL; size_t pl = 0;
                const char *body = NULL; size_t bl = 0;
                const char *name = NULL; size_t nl2 = 0;
                const char *ctype= NULL; size_t cl = 0;
                bool inline_ = false;
                obj_get_string(ao, "path", &path, &pl);
                obj_get_string(ao, "body", &body, &bl);
                obj_get_string(ao, "name", &name, &nl2);
                obj_get_string(ao, "contentType", &ctype, &cl);
                obj_get_bool(ao, "inline", &inline_);
                if (path) {
                    char pbuf[1024];
                    snprintf(pbuf, sizeof(pbuf), "%.*s", (int)pl, path);
                    FILE *fp = fopen(pbuf, "rb");
                    if (fp) {
                        char rd[4096];
                        size_t got;
                        while ((got = fread(rd, 1, sizeof(rd), fp)) > 0)
                            sb_append(&att_bodies[natt], rd, got);
                        fclose(fp);
                        atts[natt].body = att_bodies[natt].data;
                        atts[natt].body_len = att_bodies[natt].len;
                        if (!name) {
                            const char *slash = strrchr(pbuf, '/');
                            atts[natt].name = slash ? slash + 1 : pbuf;
                        } else {
                            char *nbuf = (char *)malloc(nl2 + 1);
                            memcpy(nbuf, name, nl2); nbuf[nl2] = '\0';
                            atts[natt].name = nbuf; /* leak ok -- one-shot */
                        }
                    } else {
                        atts[natt].body = NULL;
                        atts[natt].body_len = 0;
                        atts[natt].name = name ? name : "attachment";
                    }
                } else if (body) {
                    sb_append(&att_bodies[natt], body, bl);
                    atts[natt].body = att_bodies[natt].data;
                    atts[natt].body_len = att_bodies[natt].len;
                    atts[natt].name = name ? name : "attachment";
                } else {
                    continue;
                }
                atts[natt].content_type = ctype;
                atts[natt].is_inline = inline_;
                atts[natt].content_id = NULL;
                natt++;
            }
        }

        char from_buf[512];
        snprintf(from_buf, sizeof(from_buf), "%.*s", (int)froml, from);
        char to_buf[2048] = {0};
        char cc_buf[2048] = {0};
        snprintf(to_buf, sizeof(to_buf), "%.*s",
                 (int)(jt.out.len < sizeof(to_buf) - 1 ? jt.out.len : sizeof(to_buf) - 1),
                 jt.out.data ? jt.out.data : "");
        snprintf(cc_buf, sizeof(cc_buf), "%.*s",
                 (int)(jc.out.len < sizeof(cc_buf) - 1 ? jc.out.len : sizeof(cc_buf) - 1),
                 jc.out.data ? jc.out.data : "");

        char subj_buf[1024], text_buf[16] = "", html_buf[16] = "", reply_buf[256] = "";
        snprintf(subj_buf, sizeof(subj_buf), "%.*s", (int)sl, subject ? subject : "");
        snprintf(reply_buf, sizeof(reply_buf), "%.*s", (int)rl, reply_to ? reply_to : "");

        /* The mime_build_t expects NUL-terminated.  Promote text/html buffers. */
        char *text_z = NULL, *html_z = NULL;
        if (text && tl) { text_z = (char *)malloc(tl + 1); memcpy(text_z, text, tl); text_z[tl] = '\0'; }
        if (html && hl) { html_z = (char *)malloc(hl + 1); memcpy(html_z, html, hl); html_z[hl] = '\0'; }

        mime_build_t mb = {0};
        mb.from = from_buf;
        mb.to   = to_buf[0]   ? to_buf   : NULL;
        mb.cc   = cc_buf[0]   ? cc_buf   : NULL;
        mb.reply_to = reply_buf[0] ? reply_buf : NULL;
        mb.subject = subject ? subj_buf : NULL;
        mb.text = text_z;
        mb.html = html_z;
        mb.user_agent = user_agent;
        mb.attachments = atts;
        mb.n_attachments = natt;

        mime_build(&mb, &built);
        free(text_z); free(html_z);
        for (size_t i = 0; i < natt; i++) sb_free(&att_bodies[i]);

        msg_bytes = built.data; msg_len = built.len;
        (void)text_buf; (void)html_buf;
    }

    /* Optional DKIM signing.  Prepend the signature header to the body. */
    CdoObject *dkim_obj = NULL;
    if (obj_get_object(o, "dkim", &dkim_obj)) {
        const char *sel = NULL; size_t selL = 0;
        const char *dom = NULL; size_t domL = 0;
        const char *key = NULL; size_t keyL = 0;
        obj_get_string(dkim_obj, "selector", &sel, &selL);
        obj_get_string(dkim_obj, "domain",   &dom, &domL);
        obj_get_string(dkim_obj, "key",      &key, &keyL);
        if (sel && dom && key) {
            char sel_z[128] = {0}, dom_z[256] = {0};
            snprintf(sel_z, sizeof(sel_z), "%.*s", (int)selL, sel);
            snprintf(dom_z, sizeof(dom_z), "%.*s", (int)domL, dom);
            dkim_sign_in_t din = { .selector = sel_z, .domain = dom_z,
                                   .key_pem = key, .key_pem_len = keyL };
            char *hdr = dkim_sign(msg_bytes, msg_len, &din);
            if (hdr) {
                size_t hl = strlen(hdr);
                signed_blob = (char *)malloc(hl + msg_len + 1);
                memcpy(signed_blob, hdr, hl);
                memcpy(signed_blob + hl, msg_bytes, msg_len);
                signed_blob[hl + msg_len] = '\0';
                msg_bytes = signed_blob;
                msg_len   = hl + msg_len;
                free(hdr);
            }
        }
    }

    /* Establish a connection. */
    const char *server = NULL; size_t serverL = 0;
    obj_get_string(o, "server", &server, &serverL);

    char host_buf[256]; int port = 587;
    bool implicit_tls = false;
    bool starttls     = true;
    bool verify       = true;
    f64  timeout      = 30;
    obj_get_bool(o, "tls", &implicit_tls);
    bool starttls_set = obj_get_bool(o, "starttls", &starttls);
    obj_get_bool(o, "verifyPeer", &verify);
    obj_get_number(o, "timeout", &timeout);

    char err[256];

    /* Determine target hosts. */
    typedef struct { char host[256]; int port; bool implicit_tls; } target_t;
    target_t targets[8]; int nt = 0;

    if (server) {
        const char *colon = NULL;
        for (size_t i = 0; i < serverL; i++) if (server[i] == ':') colon = server + i;
        size_t hl = colon ? (size_t)(colon - server) : serverL;
        if (hl >= sizeof(host_buf)) hl = sizeof(host_buf) - 1;
        memcpy(targets[0].host, server, hl); targets[0].host[hl] = '\0';
        if (colon) {
            char pbuf[16] = {0};
            size_t pl = serverL - hl - 1;
            if (pl >= sizeof(pbuf)) pl = sizeof(pbuf) - 1;
            memcpy(pbuf, colon + 1, pl);
            port = atoi(pbuf);
        }
        targets[0].port = port;
        targets[0].implicit_tls = (port == 465) || implicit_tls;
        nt = 1;
    } else {
        /* Direct-to-MX: pick the recipient's domain from the first RCPT
         * (a real MTA splits queues per-domain; v1 keeps it simple). */
        const char *at = strchr(rcpts[0], '@');
        if (!at) {
            sb_free(&jt.out); sb_free(&jc.out); sb_free(&built);
            free(signed_blob);
            smtp_throw(vm, 0, "", "mail.send: invalid recipient address");
            return -1;
        }
        dns_mx_record_t mx[DNS_MAX_MX];
        int n = dns_lookup_mx(at + 1, mx, DNS_MAX_MX);
        if (n <= 0) {
            sb_free(&jt.out); sb_free(&jc.out); sb_free(&built);
            free(signed_blob);
            smtp_throw(vm, 0, "", "mail.send: no MX records for %s", at + 1);
            return -1;
        }
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) {
            snprintf(targets[i].host, sizeof(targets[i].host), "%s", mx[i].host);
            targets[i].port = 25;
            targets[i].implicit_tls = false;
        }
        nt = n;
        if (!starttls_set) starttls = true;
    }

    /* Try each target until one accepts the message. */
    int rc = -1;
    int last_code = 0;
    char last_err[256] = "(no targets tried)";

    for (int ti = 0; ti < nt && rc != 0; ti++) {
        sockutil_socket_t fd = sockutil_tcp_connect(
            targets[ti].host, targets[ti].port,
            AF_UNSPEC, (int)(timeout * 1000), err, sizeof(err));
        if (fd == SOCKUTIL_INVALID_SOCKET) {
            snprintf(last_err, sizeof(last_err), "%s:%d %s",
                     targets[ti].host, targets[ti].port, err);
            continue;
        }
        sockutil_set_timeout(fd, (int)(timeout * 1000));

        /* Build a temporary slot for the wire helpers. */
        SmtpSlot tmp = {0};
        tmp.fd = fd;
        snprintf(tmp.host, sizeof(tmp.host), "%s", targets[ti].host);
        tmp.port = targets[ti].port;
        linebuf_init(&tmp.rbuf);

        if (targets[ti].implicit_tls) {
            tmp.ctx = build_client_ctx(verify, err, sizeof(err));
            if (tmp.ctx) tmp.ssl = sockutil_tls_wrap(fd, tmp.ctx, true,
                                                     tmp.host, err, sizeof(err));
            if (!tmp.ssl) { sockutil_close(fd); if (tmp.ctx) SSL_CTX_free(tmp.ctx);
                            snprintf(last_err, sizeof(last_err), "TLS: %s", err);
                            continue; }
        }

        sb_t reply; sb_init(&reply);
        int gc = smtp_read_reply(&tmp, &reply);
        if (gc != 220) { last_code = gc; goto next; }
        sb_free(&reply); sb_init(&reply);

        char ehlo_buf[256];
        snprintf(ehlo_buf, sizeof(ehlo_buf), "EHLO localhost");
        gc = smtp_cmd(&tmp, ehlo_buf, &reply);
        if (gc != 250) { last_code = gc; goto next; }

        if (!targets[ti].implicit_tls && starttls &&
            reply.data && strstr(reply.data, "STARTTLS")) {
            sb_t r2; sb_init(&r2);
            int rc2 = smtp_cmd(&tmp, "STARTTLS", &r2);
            sb_free(&r2);
            if (rc2 != 220) { last_code = rc2; goto next; }
            tmp.ctx = build_client_ctx(verify, err, sizeof(err));
            if (!tmp.ctx) { snprintf(last_err, sizeof(last_err), "TLS: %s", err); goto next; }
            tmp.ssl = sockutil_tls_wrap(fd, tmp.ctx, true, tmp.host, err, sizeof(err));
            if (!tmp.ssl) { snprintf(last_err, sizeof(last_err), "TLS: %s", err); goto next; }
            sb_t r3; sb_init(&r3);
            gc = smtp_cmd(&tmp, ehlo_buf, &r3);
            sb_free(&r3);
            if (gc != 250) { last_code = gc; goto next; }
        }

        /* Auth (only if user supplied user/password). */
        const char *u = NULL; size_t ul = 0;
        const char *p = NULL; size_t pl = 0;
        obj_get_string(o, "user",     &u, &ul);
        obj_get_string(o, "password", &p, &pl);
        if (u && p && server) {
            sb_t inb; sb_init(&inb);
            sb_putc(&inb, '\0');
            sb_append(&inb, u, ul); sb_putc(&inb, '\0');
            sb_append(&inb, p, pl);
            char *enc = b64_encode_alloc((const uint8_t *)inb.data, inb.len, NULL);
            sb_free(&inb);
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "AUTH PLAIN %s", enc ? enc : "");
            free(enc);
            sb_t r4; sb_init(&r4);
            gc = smtp_cmd(&tmp, cmd, &r4);
            sb_free(&r4);
            if (gc != 235) { last_code = gc;
                snprintf(last_err, sizeof(last_err), "AUTH failed (%d)", gc);
                goto next; }
        }

        /* MAIL FROM (extract addr-only). */
        char fname[256], faddr[256];
        if (!parse_one_address(from, froml, fname, sizeof(fname),
                                faddr, sizeof(faddr))) {
            snprintf(last_err, sizeof(last_err), "from address malformed");
            goto next;
        }
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>", faddr);
        sb_t r5; sb_init(&r5);
        gc = smtp_cmd(&tmp, cmd, &r5);
        sb_free(&r5);
        if (gc != 250) { last_code = gc;
            snprintf(last_err, sizeof(last_err), "MAIL FROM rejected (%d)", gc);
            goto next; }

        /* RCPT TO (each). */
        for (size_t i = 0; i < ru.n; i++) {
            snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>", rcpts[i]);
            sb_t r6; sb_init(&r6);
            int rrc = smtp_cmd(&tmp, cmd, &r6);
            sb_free(&r6);
            if (rrc != 250 && rrc != 251) {
                last_code = rrc;
                snprintf(last_err, sizeof(last_err), "RCPT TO <%s> rejected (%d)",
                         rcpts[i], rrc);
                goto next;
            }
        }

        /* DATA. */
        sb_t r7; sb_init(&r7);
        gc = smtp_cmd(&tmp, "DATA", &r7);
        sb_free(&r7);
        if (gc != 354) { last_code = gc; goto next; }
        sb_t body_out; sb_init(&body_out);
        dot_stuff(msg_bytes, msg_len, &body_out);
        if (!slot_send_all(&tmp, body_out.data, body_out.len)) {
            sb_free(&body_out); goto next;
        }
        sb_free(&body_out);
        if (!slot_send_all(&tmp, ".\r\n", 3)) goto next;
        sb_t r8; sb_init(&r8);
        gc = smtp_read_reply(&tmp, &r8);
        sb_free(&r8);
        if (gc != 250) { last_code = gc;
            snprintf(last_err, sizeof(last_err), "DATA final reject (%d)", gc);
            goto next; }
        /* QUIT, then mark success. */
        sb_t r9; sb_init(&r9);
        smtp_cmd(&tmp, "QUIT", &r9);
        sb_free(&r9);
        rc = 0;

    next:
        if (tmp.ssl) sockutil_tls_free(tmp.ssl);
        if (tmp.ctx) SSL_CTX_free(tmp.ctx);
        sockutil_close(tmp.fd);
        linebuf_free(&tmp.rbuf);
        sb_free(&reply);
    }

    sb_free(&jt.out); sb_free(&jc.out);
    sb_free(&built);

    if (rc != 0) {
        free(signed_blob);
        smtp_throw(vm, last_code, "", "mail.send: %s", last_err);
        return -1;
    }

    /* Build result object. */
    CandoValue out_v = cando_bridge_new_object(vm);
    CdoObject *out   = cando_bridge_resolve(vm, out_v.as.handle);
    /* Extract Message-ID from the message header. */
    const char *msgid_start = NULL; size_t msgid_len = 0;
    for (size_t i = 0; i + 12 < msg_len; i++) {
        if ((i == 0 || msg_bytes[i-1] == '\n') &&
            strncasecmp(msg_bytes + i, "Message-ID:", 11) == 0) {
            const char *p = msg_bytes + i + 11;
            while (*p == ' ' || *p == '\t') p++;
            const char *e = p;
            while (e < msg_bytes + msg_len && *e != '\r' && *e != '\n') e++;
            msgid_start = p; msgid_len = (size_t)(e - p);
            break;
        }
    }
    obj_set_string(out, "messageId",
                   msgid_start ? msgid_start : "",
                   (u32)(msgid_start ? msgid_len : 0));
    /* accepted = all rcpts (we threw on partial failure). */
    CandoValue acc_v = cando_bridge_new_array(vm);
    CdoObject *acc   = cando_bridge_resolve(vm, acc_v.as.handle);
    for (size_t i = 0; i < ru.n; i++) {
        CdoString *cs = cdo_string_intern(rcpts[i], (u32)strlen(rcpts[i]));
        cdo_array_push(acc, cdo_string_value(cs));
        cdo_string_release(cs);
    }
    obj_set_array(out, "accepted", acc);
    obj_set_array(out, "rejected", cdo_array_new());

    free(signed_blob);
    cando_vm_push(vm, out_v);
    return 1;
}

/* =========================================================================
 * Parsing / building (no network)
 * ======================================================================= */

static void put_part_object(CandoVM *vm, const mime_part_t *p, CdoObject *out);

static void put_attachment_array(CandoVM *vm, const mime_part_t *root,
                                  CdoObject *arr)
{
    /* walk_attachments uses callbacks; bind via captured ud. */
    typedef struct { CandoVM *vm; CdoObject *arr; } cap_t;
    cap_t cap = { vm, arr };
    void cb(const mime_part_t *p, void *ud) { (void)0; }
    /* Simpler: inline iteration. */
    if (!root) return;
    if (root->n_children) {
        for (size_t i = 0; i < root->n_children; i++)
            put_attachment_array(vm, &root->children[i], arr);
        return;
    }
    bool is_attach = false;
    if (root->disposition && strcmp(root->disposition, "attachment") == 0) is_attach = true;
    if (root->filename && root->filename[0] != '\0') is_attach = true;
    if (root->content_type &&
        strncmp(root->content_type, "text/", 5) != 0 &&
        strncmp(root->content_type, "multipart/", 10) != 0) is_attach = true;
    if (!is_attach) return;
    CandoValue av = cando_bridge_new_object(vm);
    CdoObject *ao = cando_bridge_resolve(vm, av.as.handle);
    if (root->filename)     obj_set_string(ao, "filename",     root->filename,     (u32)strlen(root->filename));
    if (root->content_type) obj_set_string(ao, "contentType",  root->content_type, (u32)strlen(root->content_type));
    if (root->content_id)   obj_set_string(ao, "contentId",    root->content_id,   (u32)strlen(root->content_id));
    if (root->disposition)  obj_set_string(ao, "disposition",  root->disposition,  (u32)strlen(root->disposition));
    obj_set_number(ao, "size", (f64)root->body_len);
    obj_set_string(ao, "body", (const char *)root->body, (u32)root->body_len);
    cdo_array_push(arr, cdo_object_value(ao));
    (void)cap; (void)cb;
}

static void put_part_object(CandoVM *vm, const mime_part_t *p, CdoObject *out)
{
    /* headers (canonical lower-case), rawHeaders ([name,value][]) */
    CdoObject *headers = cdo_object_new();
    CdoObject *rawhdrs = cdo_array_new();
    for (mime_header_t *h = p->headers; h; h = h->next) {
        obj_set_string(headers, h->name, h->value, (u32)strlen(h->value));
        CdoObject *pair = cdo_array_new();
        CdoString *kn = cdo_string_intern(h->raw_name, (u32)strlen(h->raw_name));
        cdo_array_push(pair, cdo_string_value(kn)); cdo_string_release(kn);
        CdoString *kv = cdo_string_intern(h->raw_value, (u32)strlen(h->raw_value));
        cdo_array_push(pair, cdo_string_value(kv)); cdo_string_release(kv);
        cdo_array_push(rawhdrs, cdo_array_value(pair));
    }
    obj_set_object(out, "headers", headers);
    obj_set_array(out,  "rawHeaders", rawhdrs);

    /* text / html best-leaf. */
    const mime_part_t *t = find_text_part(p, false);
    const mime_part_t *h = find_text_part(p, true);
    if (t && t->body) obj_set_string(out, "text", (const char *)t->body, (u32)t->body_len);
    else              obj_set_string(out, "text", "", 0);
    if (h && h->body) obj_set_string(out, "html", (const char *)h->body, (u32)h->body_len);
    else              obj_set_string(out, "html", "", 0);

    /* attachments. */
    CdoObject *att = cdo_array_new();
    put_attachment_array(vm, p, att);
    obj_set_array(out, "attachments", att);
}

static int native_parse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        smtp_throw(vm, 0, "", "mail.parse: string required");
        return -1;
    }
    mime_part_t *p = mime_parse((const uint8_t *)args[0].as.string->data,
                                args[0].as.string->length);
    if (!p) {
        smtp_throw(vm, 0, "", "mail.parse: out of memory");
        return -1;
    }
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *out = cando_bridge_resolve(vm, v.as.handle);
    put_part_object(vm, p, out);
    mime_part_free(p);
    cando_vm_push(vm, v);
    return 1;
}

static int native_build(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "mail.build: opts object required");
        return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);
    const char *from = NULL; size_t froml = 0;
    obj_get_string(o, "from", &from, &froml);

    join_ud jt = { .first = true }; sb_init(&jt.out);
    visit_address_field(vm, o, "to", join_visitor, &jt);
    join_ud jc = { .first = true }; sb_init(&jc.out);
    visit_address_field(vm, o, "cc", join_visitor, &jc);

    const char *subject = NULL; size_t sl = 0;
    const char *text    = NULL; size_t tl = 0;
    const char *html    = NULL; size_t hl = 0;
    obj_get_string(o, "subject", &subject, &sl);
    obj_get_string(o, "text",    &text,    &tl);
    obj_get_string(o, "html",    &html,    &hl);

    char fbuf[512] = {0};  if (froml) snprintf(fbuf, sizeof(fbuf), "%.*s", (int)froml, from);
    char tbuf[2048]= {0};  snprintf(tbuf, sizeof(tbuf), "%.*s",
        (int)(jt.out.len < sizeof(tbuf)-1 ? jt.out.len : sizeof(tbuf)-1),
        jt.out.data ? jt.out.data : "");
    char cbuf[2048]= {0};  snprintf(cbuf, sizeof(cbuf), "%.*s",
        (int)(jc.out.len < sizeof(cbuf)-1 ? jc.out.len : sizeof(cbuf)-1),
        jc.out.data ? jc.out.data : "");
    char sbuf[1024]= {0};  if (sl) snprintf(sbuf, sizeof(sbuf), "%.*s", (int)sl, subject);

    char *text_z = NULL, *html_z = NULL;
    if (text && tl) { text_z = (char *)malloc(tl+1); memcpy(text_z, text, tl); text_z[tl]='\0'; }
    if (html && hl) { html_z = (char *)malloc(hl+1); memcpy(html_z, html, hl); html_z[hl]='\0'; }

    mime_build_t mb = {0};
    mb.from    = froml ? fbuf : NULL;
    mb.to      = tbuf[0] ? tbuf : NULL;
    mb.cc      = cbuf[0] ? cbuf : NULL;
    mb.subject = sl ? sbuf : NULL;
    mb.text    = text_z;
    mb.html    = html_z;
    mb.user_agent = "CanDo SMTP/1.0";

    sb_t out; sb_init(&out);
    mime_build(&mb, &out);
    free(text_z); free(html_z);
    sb_free(&jt.out); sb_free(&jc.out);

    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    sb_free(&out);
    return 1;
}

/* =========================================================================
 * Address helpers
 * ======================================================================= */

static int native_parseAddress(CandoVM *vm, int argc, CandoValue *args)
{
    const char *s = libutil_arg_cstr_at(args, argc, 0);
    if (!s) { smtp_throw(vm, 0, "", "mail.parseAddress: string required"); return -1; }
    size_t L = args[0].as.string->length;
    char name[256], addr[256];
    bool ok = parse_one_address(s, L, name, sizeof(name), addr, sizeof(addr));
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    obj_set_string(o, "name",    ok ? name : "", (u32)strlen(ok ? name : ""));
    obj_set_string(o, "address", ok ? addr : "", (u32)strlen(ok ? addr : ""));
    if (ok) {
        char *at = strchr(addr, '@');
        if (at) {
            *at = '\0';
            obj_set_string(o, "local",  addr, (u32)strlen(addr));
            obj_set_string(o, "domain", at+1, (u32)strlen(at+1));
        }
    }
    cando_vm_push(vm, v);
    return 1;
}

static bool parselist_cb(const char *item, size_t n, void *ud)
{
    CdoObject *arr = (CdoObject *)ud;
    /* Trim whitespace. */
    while (n && (item[0] == ' ' || item[0] == '\t')) { item++; n--; }
    while (n && (item[n-1] == ' ' || item[n-1] == '\t' ||
                 item[n-1] == '\r' || item[n-1] == '\n')) n--;
    if (!n) return true;
    char name[256], addr[256];
    if (parse_one_address(item, n, name, sizeof(name), addr, sizeof(addr))) {
        CdoObject *e = cdo_object_new();
        obj_set_string(e, "name",    name, (u32)strlen(name));
        obj_set_string(e, "address", addr, (u32)strlen(addr));
        cdo_array_push(arr, cdo_object_value(e));
    }
    return true;
}

static int native_parseAddressList(CandoVM *vm, int argc, CandoValue *args)
{
    const char *s = libutil_arg_cstr_at(args, argc, 0);
    if (!s) { smtp_throw(vm, 0, "", "mail.parseAddressList: string required"); return -1; }
    size_t L = args[0].as.string->length;
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    split_addr_list(s, L, parselist_cb, a);
    cando_vm_push(vm, v);
    return 1;
}

static int native_formatAddress(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "mail.formatAddress: object required"); return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);
    const char *name = NULL; size_t nl = 0;
    const char *addr = NULL; size_t al = 0;
    obj_get_string(o, "name",    &name, &nl);
    obj_get_string(o, "address", &addr, &al);
    sb_t out; sb_init(&out);
    if (name && nl) {
        bool need_quote = false;
        for (size_t i = 0; i < nl; i++) {
            char c = name[i];
            if (c == ',' || c == '"' || c == '<' || c == '>' || c == '@') {
                need_quote = true; break;
            }
        }
        if (need_quote) {
            sb_putc(&out, '"');
            for (size_t i = 0; i < nl; i++) {
                if (name[i] == '"' || name[i] == '\\') sb_putc(&out, '\\');
                sb_putc(&out, name[i]);
            }
            sb_putc(&out, '"');
        } else {
            sb_append(&out, name, nl);
        }
        sb_putc(&out, ' ');
        sb_putc(&out, '<'); sb_append(&out, addr ? addr : "", al); sb_putc(&out, '>');
    } else {
        sb_append(&out, addr ? addr : "", al);
    }
    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    sb_free(&out);
    return 1;
}

static int native_encodeHeader(CandoVM *vm, int argc, CandoValue *args)
{
    const char *s = libutil_arg_cstr_at(args, argc, 0);
    if (!s) { smtp_throw(vm, 0, "", "mail.encodeHeader: string required"); return -1; }
    size_t L = args[0].as.string->length;
    sb_t out; sb_init(&out);
    rfc2047_encode_q(s, L, &out);
    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    sb_free(&out);
    return 1;
}

static int native_decodeHeader(CandoVM *vm, int argc, CandoValue *args)
{
    const char *s = libutil_arg_cstr_at(args, argc, 0);
    if (!s) { smtp_throw(vm, 0, "", "mail.decodeHeader: string required"); return -1; }
    size_t L = args[0].as.string->length;
    sb_t out; sb_init(&out);
    rfc2047_decode(s, L, &out);
    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    sb_free(&out);
    return 1;
}

/* =========================================================================
 * DNS helpers
 * ======================================================================= */

static int native_mx(CandoVM *vm, int argc, CandoValue *args)
{
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    if (!name) { smtp_throw(vm, 0, "", "mail.mx: domain string required"); return -1; }
    dns_mx_record_t mx[DNS_MAX_MX];
    int n = dns_lookup_mx(name, mx, DNS_MAX_MX);
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    for (int i = 0; i < n; i++) {
        CdoObject *e = cdo_object_new();
        obj_set_number(e, "priority", (f64)mx[i].priority);
        obj_set_string(e, "host", mx[i].host, (u32)strlen(mx[i].host));
        cdo_array_push(a, cdo_object_value(e));
    }
    cando_vm_push(vm, v);
    return 1;
}

static int native_txt(CandoVM *vm, int argc, CandoValue *args)
{
    const char *name = libutil_arg_cstr_at(args, argc, 0);
    if (!name) { smtp_throw(vm, 0, "", "mail.txt: domain string required"); return -1; }
    dns_txt_record_t txt[DNS_MAX_TXT];
    int n = dns_lookup_txt(name, txt, DNS_MAX_TXT);
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    for (int i = 0; i < n; i++) {
        CdoString *s = cdo_string_intern(txt[i].text, (u32)strlen(txt[i].text));
        cdo_array_push(a, cdo_string_value(s));
        cdo_string_release(s);
    }
    cando_vm_push(vm, v);
    return 1;
}

static int native_ptr(CandoVM *vm, int argc, CandoValue *args)
{
    const char *ip = libutil_arg_cstr_at(args, argc, 0);
    if (!ip) { smtp_throw(vm, 0, "", "mail.ptr: ip string required"); return -1; }
    dns_ptr_record_t ptr[DNS_MAX_PTR];
    int n = dns_lookup_ptr(ip, ptr, DNS_MAX_PTR);
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    for (int i = 0; i < n; i++) {
        CdoString *s = cdo_string_intern(ptr[i].host, (u32)strlen(ptr[i].host));
        cdo_array_push(a, cdo_string_value(s));
        cdo_string_release(s);
    }
    cando_vm_push(vm, v);
    return 1;
}

/* =========================================================================
 * SPF
 * ======================================================================= */

static int native_spfCheck(CandoVM *vm, int argc, CandoValue *args)
{
    const char *sender = libutil_arg_cstr_at(args, argc, 0);
    const char *ip     = libutil_arg_cstr_at(args, argc, 1);
    if (!sender || !ip) {
        smtp_throw(vm, 0, "", "mail.spfCheck: (sender, ip) required");
        return -1;
    }
    const char *r = spf_check(sender, ip);
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    obj_set_string(o, "result", r, (u32)strlen(r));
    cando_vm_push(vm, v);
    return 1;
}

/* =========================================================================
 * DKIM
 * ======================================================================= */

static int native_dkimSign(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_object(args[1])) {
        smtp_throw(vm, 0, "", "mail.dkimSign: (raw_message, opts) required");
        return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[1].as.handle);
    const char *sel = NULL; size_t selL = 0;
    const char *dom = NULL; size_t domL = 0;
    const char *key = NULL; size_t keyL = 0;
    obj_get_string(o, "selector", &sel, &selL);
    obj_get_string(o, "domain",   &dom, &domL);
    obj_get_string(o, "key",      &key, &keyL);
    if (!sel || !dom || !key) {
        smtp_throw(vm, 0, "", "mail.dkimSign: selector/domain/key required");
        return -1;
    }
    char sel_z[128] = {0}, dom_z[256] = {0};
    snprintf(sel_z, sizeof(sel_z), "%.*s", (int)selL, sel);
    snprintf(dom_z, sizeof(dom_z), "%.*s", (int)domL, dom);
    dkim_sign_in_t din = { .selector = sel_z, .domain = dom_z,
                           .key_pem = key, .key_pem_len = keyL };
    char *hdr = dkim_sign(args[0].as.string->data, args[0].as.string->length, &din);
    if (!hdr) {
        smtp_throw(vm, 0, "", "mail.dkimSign: signing failed");
        return -1;
    }
    sb_t out; sb_init(&out);
    sb_puts(&out, hdr);
    sb_append(&out, args[0].as.string->data, args[0].as.string->length);
    free(hdr);
    libutil_push_str(vm, out.data, (u32)out.len);
    sb_free(&out);
    return 1;
}

static int native_dkimVerify(CandoVM *vm, int argc, CandoValue *args)
{
    const char *raw = libutil_arg_cstr_at(args, argc, 0);
    if (!raw) { smtp_throw(vm, 0, "", "mail.dkimVerify: string required"); return -1; }
    size_t L = args[0].as.string->length;
    dkim_verify_result_t r = dkim_verify(raw, L);
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    obj_set_bool  (o, "pass",     r.pass);
    obj_set_string(o, "domain",   r.domain,   (u32)strlen(r.domain));
    obj_set_string(o, "selector", r.selector, (u32)strlen(r.selector));
    obj_set_string(o, "reason",   r.reason ? r.reason : "",
                   (u32)(r.reason ? strlen(r.reason) : 0));
    cando_vm_push(vm, v);
    return 1;
}

/* =========================================================================
 * Storage helpers
 * ======================================================================= */

static int native_deliverMaildir(CandoVM *vm, int argc, CandoValue *args)
{
    const char *raw = libutil_arg_cstr_at(args, argc, 0);
    const char *dir = libutil_arg_cstr_at(args, argc, 1);
    if (!raw || !dir) {
        smtp_throw(vm, 0, "", "mail.deliverMaildir: (raw_message, maildir_path) required");
        return -1;
    }
    if (maildir_deliver(dir, (const uint8_t *)raw,
                        args[0].as.string->length) != 0) {
        smtp_throw(vm, 0, "", "mail.deliverMaildir: write failed");
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_deliverMbox(CandoVM *vm, int argc, CandoValue *args)
{
    const char *raw = libutil_arg_cstr_at(args, argc, 0);
    const char *path = libutil_arg_cstr_at(args, argc, 1);
    const char *env  = libutil_arg_cstr_at(args, argc, 2);
    if (!raw || !path) {
        smtp_throw(vm, 0, "", "mail.deliverMbox: (raw_message, mbox_path[, envelope_from]) required");
        return -1;
    }
    if (mbox_deliver(path, env ? env : "MAILER-DAEMON",
                     (const uint8_t *)raw,
                     args[0].as.string->length) != 0) {
        smtp_throw(vm, 0, "", "mail.deliverMbox: write failed");
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * POP3 client (minimal)
 * ======================================================================= */

static SmtpSlot *open_line_session(CandoVM *vm, CdoObject *o, SmtpKind kind,
                                    const char *fn, int default_port)
{
    const char *host = NULL; size_t hl = 0;
    f64 port = default_port;
    bool tls = false;
    bool verify = true;
    f64 timeout = 30;
    obj_get_string(o, "host", &host, &hl);
    obj_get_number(o, "port", &port);
    obj_get_bool(o, "tls", &tls);
    obj_get_bool(o, "verifyPeer", &verify);
    obj_get_number(o, "timeout", &timeout);
    if (!host) { smtp_throw(vm, 0, "", "%s: host required", fn); return NULL; }

    int slot = pool_alloc(kind);
    if (slot < 0) { smtp_throw(vm, 0, "", "%s: pool exhausted", fn); return NULL; }
    SmtpSlot *s = &g_smtp_pool[slot];
    snprintf(s->host, sizeof(s->host), "%.*s", (int)hl, host);
    s->port = (int)port;
    char err[256] = {0};
    s->fd = sockutil_tcp_connect(s->host, s->port, AF_UNSPEC,
                                  (int)(timeout * 1000), err, sizeof(err));
    if (s->fd == SOCKUTIL_INVALID_SOCKET) {
        pool_release(slot);
        smtp_throw(vm, 0, "", "%s: connect %s:%d: %s",
                   fn, s->host, s->port, err);
        return NULL;
    }
    sockutil_set_timeout(s->fd, (int)(timeout * 1000));
    if (tls) {
        s->ctx = build_client_ctx(verify, err, sizeof(err));
        if (!s->ctx) { pool_release(slot);
            smtp_throw(vm, 0, "", "%s: TLS ctx: %s", fn, err); return NULL; }
        s->ssl = sockutil_tls_wrap(s->fd, s->ctx, true, s->host, err, sizeof(err));
        if (!s->ssl) { pool_release(slot);
            smtp_throw(vm, 0, "", "%s: TLS wrap: %s", fn, err); return NULL; }
    }
    /* POP3 / IMAP greeting consumed by callers. */
    return s;
}

static int native_popConnect(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "mail.popConnect: opts object required"); return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);
    SmtpSlot *s = open_line_session(vm, o, SMTP_KIND_POP3, "mail.popConnect", 995);
    if (!s) return -1;

    /* Greeting +OK ... */
    sb_t line; sb_init(&line);
    if (slot_read_line(s, &line) != 0 ||
        line.len < 3 || strncmp(line.data, "+OK", 3) != 0) {
        sb_free(&line);
        smtp_throw(vm, 0, "", "mail.popConnect: bad greeting");
        return -1;
    }
    sb_free(&line);

    /* USER / PASS. */
    const char *user = NULL, *pwd = NULL;
    size_t ul = 0, pl = 0;
    obj_get_string(o, "user",     &user, &ul);
    obj_get_string(o, "password", &pwd,  &pl);
    if (user && pwd) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "USER %.*s\r\n", (int)ul, user);
        slot_send_all(s, cmd, strlen(cmd));
        sb_t l; sb_init(&l);
        slot_read_line(s, &l);
        if (l.len < 3 || strncmp(l.data, "+OK", 3) != 0) {
            sb_free(&l); smtp_throw(vm, 0, "", "mail.popConnect: USER rejected");
            return -1;
        }
        sb_free(&l); sb_init(&l);
        snprintf(cmd, sizeof(cmd), "PASS %.*s\r\n", (int)pl, pwd);
        slot_send_all(s, cmd, strlen(cmd));
        slot_read_line(s, &l);
        if (l.len < 3 || strncmp(l.data, "+OK", 3) != 0) {
            sb_free(&l); smtp_throw(vm, 0, "", "mail.popConnect: PASS rejected");
            return -1;
        }
        sb_free(&l);
    }
    int slot_idx = (int)(s - g_smtp_pool);
    cando_vm_push(vm, make_handle(vm, slot_idx));
    return 1;
}

static int native_popList(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "mail.popList: session required"); return -1; }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_POP3, "mail.popList");
    if (!s) return -1;
    slot_send_all(s, "LIST\r\n", 6);
    sb_t status; sb_init(&status);
    if (slot_read_line(s, &status) != 0 ||
        status.len < 3 || strncmp(status.data, "+OK", 3) != 0) {
        sb_free(&status); smtp_throw(vm, 0, "", "mail.popList: LIST rejected"); return -1;
    }
    sb_free(&status);
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    while (1) {
        sb_t l; sb_init(&l);
        if (slot_read_line(s, &l) != 0) { sb_free(&l); break; }
        if (l.len == 1 && l.data[0] == '.') { sb_free(&l); break; }
        int n = 0, sz = 0;
        if (sscanf(l.data, "%d %d", &n, &sz) == 2) {
            CdoObject *e = cdo_object_new();
            obj_set_number(e, "n",    (f64)n);
            obj_set_number(e, "size", (f64)sz);
            cdo_array_push(a, cdo_object_value(e));
        }
        sb_free(&l);
    }
    cando_vm_push(vm, v);
    return 1;
}

static int native_popRetr(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_number(args[1])) {
        smtp_throw(vm, 0, "", "mail.popRetr: (session, n) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_POP3, "mail.popRetr");
    if (!s) return -1;
    int n = (int)args[1].as.number;
    char cmd[64]; snprintf(cmd, sizeof(cmd), "RETR %d\r\n", n);
    slot_send_all(s, cmd, strlen(cmd));
    sb_t status; sb_init(&status);
    if (slot_read_line(s, &status) != 0 ||
        status.len < 3 || strncmp(status.data, "+OK", 3) != 0) {
        sb_free(&status); smtp_throw(vm, 0, "", "mail.popRetr: RETR rejected"); return -1;
    }
    sb_free(&status);
    sb_t body; sb_init(&body);
    while (1) {
        sb_t l; sb_init(&l);
        if (slot_read_line(s, &l) != 0) { sb_free(&l); break; }
        if (l.len == 1 && l.data[0] == '.') { sb_free(&l); break; }
        const char *src = l.data; size_t L = l.len;
        if (L >= 2 && src[0] == '.' && src[1] == '.') { src++; L--; }
        sb_append(&body, src, L);
        sb_puts(&body, "\r\n");
        sb_free(&l);
    }
    libutil_push_str(vm, body.data ? body.data : "", (u32)body.len);
    sb_free(&body);
    return 1;
}

static int native_popDele(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_number(args[1])) {
        smtp_throw(vm, 0, "", "mail.popDele: (session, n) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_POP3, "mail.popDele");
    if (!s) return -1;
    int n = (int)args[1].as.number;
    char cmd[64]; snprintf(cmd, sizeof(cmd), "DELE %d\r\n", n);
    slot_send_all(s, cmd, strlen(cmd));
    sb_t l; sb_init(&l);
    slot_read_line(s, &l);
    bool ok = (l.len >= 3 && strncmp(l.data, "+OK", 3) == 0);
    sb_free(&l);
    cando_vm_push(vm, cando_bool(ok));
    return 1;
}

static int native_popQuit(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "mail.popQuit: session required"); return -1; }
    int slot = handle_slot(vm, args[0]);
    if (slot >= 0 && g_smtp_pool[slot].in_use &&
        g_smtp_pool[slot].kind == SMTP_KIND_POP3) {
        slot_send_all(&g_smtp_pool[slot], "QUIT\r\n", 6);
        sb_t l; sb_init(&l); slot_read_line(&g_smtp_pool[slot], &l); sb_free(&l);
    }
    handle_mark_closed(vm, args[0]);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * IMAP client (narrow surface)
 * ======================================================================= */

static int imap_send(SmtpSlot *s, const char *cmd, char tag_out[16])
{
    s->imap_seq++;
    snprintf(tag_out, 16, "A%04d", s->imap_seq);
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "%s %s\r\n", tag_out, cmd);
    return slot_send_all(s, buf, (size_t)n) ? 0 : -1;
}

/* Read lines until we see one starting with "<tag> OK|NO|BAD".
 * Untagged responses go into out_lines (newline-joined). */
static int imap_read_until(SmtpSlot *s, const char *tag, sb_t *out_lines)
{
    while (1) {
        sb_t l; sb_init(&l);
        if (slot_read_line(s, &l) != 0) { sb_free(&l); return -1; }
        if (l.len > strlen(tag) && strncmp(l.data, tag, strlen(tag)) == 0 &&
            l.data[strlen(tag)] == ' ') {
            const char *p = l.data + strlen(tag) + 1;
            int rc = -1;
            if      (strncmp(p, "OK",  2) == 0) rc = 0;
            else if (strncmp(p, "NO",  2) == 0) rc = 1;
            else if (strncmp(p, "BAD", 3) == 0) rc = 2;
            sb_free(&l);
            return rc;
        }
        if (out_lines->len) sb_putc(out_lines, '\n');
        sb_append(out_lines, l.data, l.len);
        sb_free(&l);
    }
}

static int native_imapConnect(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        smtp_throw(vm, 0, "", "mail.imapConnect: opts required"); return -1;
    }
    CdoObject *o = cando_bridge_resolve(vm, args[0].as.handle);
    SmtpSlot *s = open_line_session(vm, o, SMTP_KIND_IMAP, "mail.imapConnect", 993);
    if (!s) return -1;
    /* Greeting "* OK ..." */
    sb_t l; sb_init(&l);
    if (slot_read_line(s, &l) != 0 || l.len < 4 || l.data[0] != '*') {
        sb_free(&l); smtp_throw(vm, 0, "", "mail.imapConnect: bad greeting"); return -1;
    }
    sb_free(&l);

    /* LOGIN. */
    const char *user = NULL, *pwd = NULL;
    size_t ul = 0, pl = 0;
    obj_get_string(o, "user",     &user, &ul);
    obj_get_string(o, "password", &pwd,  &pl);
    if (user && pwd) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "LOGIN \"%.*s\" \"%.*s\"",
                 (int)ul, user, (int)pl, pwd);
        char tag[16];
        imap_send(s, cmd, tag);
        sb_t resp; sb_init(&resp);
        int rc = imap_read_until(s, tag, &resp);
        sb_free(&resp);
        if (rc != 0) {
            int slot_idx = (int)(s - g_smtp_pool);
            pool_release(slot_idx);
            smtp_throw(vm, 0, "", "mail.imapConnect: LOGIN failed");
            return -1;
        }
    }
    int slot_idx = (int)(s - g_smtp_pool);
    cando_vm_push(vm, make_handle(vm, slot_idx));
    return 1;
}

static int native_imapSelect(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[1])) {
        smtp_throw(vm, 0, "", "mail.imapSelect: (session, mailbox) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_IMAP, "mail.imapSelect");
    if (!s) return -1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "SELECT \"%.*s\"",
             (int)args[1].as.string->length, args[1].as.string->data);
    char tag[16]; imap_send(s, cmd, tag);
    sb_t r; sb_init(&r);
    int rc = imap_read_until(s, tag, &r);
    sb_free(&r);
    if (rc != 0) { smtp_throw(vm, 0, "", "mail.imapSelect: failed"); return -1; }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_imapSearch(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[1])) {
        smtp_throw(vm, 0, "", "mail.imapSearch: (session, query) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_IMAP, "mail.imapSearch");
    if (!s) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "UID SEARCH %.*s",
             (int)args[1].as.string->length, args[1].as.string->data);
    char tag[16]; imap_send(s, cmd, tag);
    sb_t r; sb_init(&r);
    int rc = imap_read_until(s, tag, &r);
    if (rc != 0) { sb_free(&r); smtp_throw(vm, 0, "", "mail.imapSearch: failed"); return -1; }
    /* Untagged responses look like "* SEARCH 1 2 3 4". */
    CandoValue v = cando_bridge_new_array(vm);
    CdoObject *a = cando_bridge_resolve(vm, v.as.handle);
    const char *p = r.data ? r.data : "";
    while ((p = strstr(p, "* SEARCH"))) {
        p += 8;
        while (*p == ' ') p++;
        while (*p && *p != '\n') {
            char *e = NULL;
            long n = strtol(p, &e, 10);
            if (e == p) break;
            cdo_array_push(a, cdo_number((f64)n));
            p = e;
            while (*p == ' ') p++;
        }
    }
    sb_free(&r);
    cando_vm_push(vm, v);
    return 1;
}

static int native_imapFetch(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_number(args[1])) {
        smtp_throw(vm, 0, "", "mail.imapFetch: (session, uid[, item]) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_IMAP, "mail.imapFetch");
    if (!s) return -1;
    int uid = (int)args[1].as.number;
    const char *item = libutil_arg_cstr_at(args, argc, 2);
    if (!item) item = "RFC822";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "UID FETCH %d %s", uid, item);
    char tag[16]; imap_send(s, cmd, tag);

    /* The response carries a literal {N}\r\n then N bytes.  We need to
     * consume that literal across line boundaries. */
    sb_t out; sb_init(&out);
    while (1) {
        sb_t l; sb_init(&l);
        if (slot_read_line(s, &l) != 0) { sb_free(&l); break; }
        if (strncmp(l.data, tag, strlen(tag)) == 0 &&
            l.data[strlen(tag)] == ' ') {
            sb_free(&l); break;
        }
        /* Literal? */
        const char *brace = strchr(l.data, '{');
        if (brace) {
            size_t need = (size_t)atoi(brace + 1);
            sb_free(&l);
            /* Read `need` bytes from the connection (account for what's
             * already in linebuf). */
            while (out.len < need) {
                size_t avail = s->rbuf.buf.len - s->rbuf.consumed;
                if (avail) {
                    size_t take = avail < (need - out.len) ? avail : (need - out.len);
                    sb_append(&out, s->rbuf.buf.data + s->rbuf.consumed, take);
                    s->rbuf.consumed += take;
                } else {
                    char tmp[4096];
                    int k = slot_recv(s, tmp, sizeof(tmp));
                    if (k <= 0) break;
                    sb_append(&s->rbuf.buf, tmp, (size_t)k);
                }
            }
            /* Consume trailing CRLF after literal. */
            sb_t tail; sb_init(&tail);
            slot_read_line(s, &tail);
            sb_free(&tail);
            continue;
        }
        sb_free(&l);
    }
    libutil_push_str(vm, out.data ? out.data : "", (u32)out.len);
    sb_free(&out);
    return 1;
}

static int imap_simple(CandoVM *vm, int argc, CandoValue *args, const char *fn,
                       const char *cmd_fmt)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "%s: session required", fn); return -1; }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_IMAP, fn);
    if (!s) return -1;
    char cmd[1024];
    if (argc >= 2 && cando_is_string(args[1])) {
        snprintf(cmd, sizeof(cmd), cmd_fmt,
                 (int)args[1].as.string->length, args[1].as.string->data);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", cmd_fmt);
    }
    char tag[16]; imap_send(s, cmd, tag);
    sb_t r; sb_init(&r);
    int rc = imap_read_until(s, tag, &r);
    sb_free(&r);
    if (rc != 0) { smtp_throw(vm, 0, "", "%s: failed", fn); return -1; }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_imapMove(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3 || !cando_is_number(args[1]) || !cando_is_string(args[2])) {
        smtp_throw(vm, 0, "", "mail.imapMove: (session, uid, mailbox) required"); return -1;
    }
    SmtpSlot *s = handle_unwrap(vm, args[0], SMTP_KIND_IMAP, "mail.imapMove");
    if (!s) return -1;
    int uid = (int)args[1].as.number;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "UID MOVE %d \"%.*s\"", uid,
             (int)args[2].as.string->length, args[2].as.string->data);
    char tag[16]; imap_send(s, cmd, tag);
    sb_t r; sb_init(&r);
    int rc = imap_read_until(s, tag, &r);
    sb_free(&r);
    if (rc != 0) {
        /* Fallback: COPY + STORE \Deleted + EXPUNGE for servers without MOVE. */
        snprintf(cmd, sizeof(cmd), "UID COPY %d \"%.*s\"", uid,
                 (int)args[2].as.string->length, args[2].as.string->data);
        imap_send(s, cmd, tag);
        sb_t r2; sb_init(&r2); rc = imap_read_until(s, tag, &r2); sb_free(&r2);
        if (rc != 0) { smtp_throw(vm, 0, "", "mail.imapMove: COPY failed"); return -1; }
        snprintf(cmd, sizeof(cmd), "UID STORE %d +FLAGS (\\Deleted)", uid);
        imap_send(s, cmd, tag);
        sb_t r3; sb_init(&r3); imap_read_until(s, tag, &r3); sb_free(&r3);
        imap_send(s, "EXPUNGE", tag);
        sb_t r4; sb_init(&r4); imap_read_until(s, tag, &r4); sb_free(&r4);
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

static int native_imapLogout(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) { smtp_throw(vm, 0, "", "mail.imapLogout: session required"); return -1; }
    int slot = handle_slot(vm, args[0]);
    if (slot >= 0 && g_smtp_pool[slot].in_use &&
        g_smtp_pool[slot].kind == SMTP_KIND_IMAP) {
        char tag[16]; imap_send(&g_smtp_pool[slot], "LOGOUT", tag);
        sb_t r; sb_init(&r); imap_read_until(&g_smtp_pool[slot], tag, &r); sb_free(&r);
    }
    handle_mark_closed(vm, args[0]);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Constants
 * ======================================================================= */

static void register_constants(CdoObject *obj)
{
    obj_set_number(obj, "PORT_SMTP",        25);
    obj_set_number(obj, "PORT_SUBMISSION", 587);
    obj_set_number(obj, "PORT_SMTPS",      465);
    obj_set_number(obj, "PORT_POP3",       110);
    obj_set_number(obj, "PORT_POP3S",      995);
    obj_set_number(obj, "PORT_IMAP",       143);
    obj_set_number(obj, "PORT_IMAPS",      993);
    obj_set_number(obj, "MAX_LINE",        998);
    obj_set_string(obj, "VERSION",         "1.0.0", 5);
}

/* =========================================================================
 * Module registration
 * ======================================================================= */

static const LibutilMethodEntry smtp_methods[] = {
    /* high-level send */
    { "send",                native_send             },
    /* low-level SMTP */
    { "connect",             native_connect          },
    { "capabilities",        native_capabilities     },
    { "mailFrom",            native_mail_from        },
    { "rcptTo",              native_rcpt_to          },
    { "data",                native_data             },
    { "reset",               native_reset            },
    { "noop",                native_noop             },
    { "close",               native_close            },
    /* MIME */
    { "parse",               native_parse            },
    { "build",               native_build            },
    /* address helpers */
    { "parseAddress",        native_parseAddress     },
    { "parseAddressList",    native_parseAddressList },
    { "formatAddress",       native_formatAddress    },
    { "encodeHeader",        native_encodeHeader     },
    { "decodeHeader",        native_decodeHeader     },
    /* DNS */
    { "mx",                  native_mx               },
    { "txt",                 native_txt              },
    { "ptr",                 native_ptr              },
    /* SPF */
    { "spfCheck",            native_spfCheck         },
    /* DKIM */
    { "dkimSign",            native_dkimSign         },
    { "dkimVerify",          native_dkimVerify       },
    /* Storage */
    { "deliverMaildir",      native_deliverMaildir   },
    { "deliverMbox",         native_deliverMbox      },
    /* POP3 */
    { "popConnect",          native_popConnect       },
    { "popList",             native_popList          },
    { "popRetr",             native_popRetr          },
    { "popDele",             native_popDele          },
    { "popQuit",             native_popQuit          },
    /* IMAP */
    { "imapConnect",         native_imapConnect      },
    { "imapSelect",          native_imapSelect       },
    { "imapSearch",          native_imapSearch       },
    { "imapFetch",           native_imapFetch        },
    { "imapMove",            native_imapMove         },
    { "imapLogout",          native_imapLogout       },
};

/* Build the module table.  Used by both cando_module_init (for
 * include()) and cando_open_smtplib (for embedders). */
static CandoValue build_module_table(CandoVM *vm)
{
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);
    libutil_register_methods(vm, obj, smtp_methods,
                              sizeof(smtp_methods)/sizeof(*smtp_methods));
    register_constants(obj);
    return tbl;
}

#if defined(_WIN32) || defined(_WIN64)
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
CandoValue cando_module_init(CandoVM *vm)
{
    return build_module_table(vm);
}

/* Embedder-side hook: register `mail` global with the same surface. */
CANDO_API void cando_open_smtplib(CandoVM *vm);
CANDO_API void cando_open_smtplib(CandoVM *vm)
{
    CandoValue tbl = build_module_table(vm);
    cando_vm_set_global(vm, "mail", tbl, /*is_const=*/true);
}
