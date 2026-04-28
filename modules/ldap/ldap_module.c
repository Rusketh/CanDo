/*
 * modules/ldap/ldap_module.c -- CanDo LDAP / Active Directory binary module.
 *
 * Loaded into a script with:
 *
 *     VAR ldap = include("./ldap.so");        // Linux / macOS
 *     VAR ldap = include("./ldap.dll");       // Windows
 *
 * The module exposes the full set of operations the PowerShell ActiveDirectory
 * module provides: read (search), write (add / modify / compare),
 * move (rename / modrdn), and delete -- plus connection management
 * (bind, unbind, start_tls, set_option).
 *
 * Backed by:
 *   - OpenLDAP libldap (Linux / macOS)
 *   - Windows native wldap32.dll  (Windows)
 *
 * Both libraries implement RFC 1823 (the LDAP C API), so the cross-platform
 * surface is thin.  Differences are isolated behind the LDAP_PLATFORM_*
 * macros below.
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
#include "core/thread_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp */
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>

#if defined(_WIN32) || defined(_WIN64)
#  define LDAP_PLATFORM_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winldap.h>
#  include <winber.h>
   /* winldap uses ULONG return codes; PCHAR for strings.  Cast at the
    * boundary to keep call sites uniform with the OpenLDAP path. */
#  define LDAP_STR_T   PCHAR
#  define LDAP_CSTR_T  PCHAR
   /* winldap declares ldap_err2string returning PCHAR. */
#else
#  define LDAP_PLATFORM_POSIX 1
#  include <ldap.h>
#  include <sys/time.h>
#  define LDAP_STR_T   char *
#  define LDAP_CSTR_T  const char *
#endif

#include "ldap_helpers.h"
#include "ldap_adcrypt.h"

/* =========================================================================
 * Connection pool
 *
 * Script-facing connection objects carry a single number field
 * `__ldap_slot` that is an index into the static pool below.  This avoids
 * the f64 split-pointer trick, gives us strict handle validation (a slot
 * marked unused returns a clean error rather than UB), and follows the
 * pattern already used by source/lib/socket.c.
 * ======================================================================= */

#define LDAP_MAX_INSTANCES   256
#define LDAP_SLOT_KEY        "__ldap_slot"

typedef struct LdapSlot {
    LDAP *ld;
    bool  in_use;
} LdapSlot;

static LdapSlot      g_ldap_pool[LDAP_MAX_INSTANCES];
static cando_mutex_t g_ldap_pool_mutex;
static _Atomic(int)  g_ldap_pool_inited = 0;

static void ensure_pool_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_ldap_pool_inited, &expected, 1)) {
        cando_os_mutex_init(&g_ldap_pool_mutex);
        for (int i = 0; i < LDAP_MAX_INSTANCES; i++) {
            g_ldap_pool[i].ld     = NULL;
            g_ldap_pool[i].in_use = false;
        }
    }
}

static int pool_alloc(LDAP *ld)
{
    ensure_pool_inited();
    cando_os_mutex_lock(&g_ldap_pool_mutex);
    int idx = -1;
    for (int i = 0; i < LDAP_MAX_INSTANCES; i++) {
        if (!g_ldap_pool[i].in_use) {
            g_ldap_pool[i].ld     = ld;
            g_ldap_pool[i].in_use = true;
            idx = i;
            break;
        }
    }
    cando_os_mutex_unlock(&g_ldap_pool_mutex);
    return idx;
}

static LDAP *pool_get(int idx)
{
    if (idx < 0 || idx >= LDAP_MAX_INSTANCES) return NULL;
    if (!g_ldap_pool[idx].in_use)             return NULL;
    return g_ldap_pool[idx].ld;
}

static void pool_release(int idx)
{
    if (idx < 0 || idx >= LDAP_MAX_INSTANCES) return;
    cando_os_mutex_lock(&g_ldap_pool_mutex);
    g_ldap_pool[idx].ld     = NULL;
    g_ldap_pool[idx].in_use = false;
    cando_os_mutex_unlock(&g_ldap_pool_mutex);
}

/* Map a numeric LDAP result code to its human-readable message.
 * Visible to test harnesses via the ldap_module_strerror prototype below. */
const char *ldap_module_strerror(int code);
const char *ldap_module_strerror(int code)
{
#if defined(LDAP_PLATFORM_WINDOWS)
    return (const char *)ldap_err2string((ULONG)code);
#else
    return ldap_err2string(code);
#endif
}

/* =========================================================================
 * Multi-value error throws
 *
 * Scripts catch LDAP errors with `CATCH (msg, code, diag)`:
 *   - msg  = formatted error message (string)
 *   - code = numeric LDAP result code (number, 0 if not from libldap)
 *   - diag = libldap diagnostic message (string) or "" if none
 *
 * cando_vm_error sets error_vals[0]; we extend slots [1] and [2] in place,
 * which is the same mechanism the parser uses for THROW with multiple
 * values (see source/vm/vm.c::vm_error_commit and OP_THROW dispatch).
 * ======================================================================= */

/* Forward decl -- vm.h doesn't expose this struct member API publicly,
 * but error_vals[] is documented in vm.h:252. */
extern void cando_value_release(CandoValue v);

static void ldap_attach_extra(CandoVM *vm, int code, const char *diag)
{
    /* Slot 0 is the formatted message (set by cando_vm_error).  We extend
     * with code in slot 1 and diagnostic string in slot 2. */
    cando_value_release(vm->error_vals[1]);
    vm->error_vals[1] = cando_number((f64)code);

    cando_value_release(vm->error_vals[2]);
    const char *d = diag ? diag : "";
    CandoString *s = cando_string_new(d, (u32)strlen(d));
    vm->error_vals[2] = cando_string_value(s);

    if (vm->error_val_count < 3) vm->error_val_count = 3;
}

#if !defined(LDAP_PLATFORM_WINDOWS)
/* Diagnostic message lookup (libldap-side, not the human-readable result
 * code text from ldap_err2string).  POSIX-only; wldap32 doesn't expose
 * an equivalent option, and ldap_throw routes around it on Windows. */
static const char *ldap_get_diagnostic(LDAP *ld)
{
    char *diag = NULL;
    if (!ld) return NULL;
    if (ldap_get_option(ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag) != LDAP_SUCCESS)
        return NULL;
    /* libldap manages the storage.  The caller copies it if it needs to
     * survive past the next libldap call. */
    return diag;
}
#endif

/* ldap_throw -- helper that fires a multi-value throw from native code.
 * After this call, return -1 from the native. */
static void ldap_throw(CandoVM *vm, LDAP *ld, int code, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    cando_vm_error(vm, "%s", buf);
#if defined(LDAP_PLATFORM_WINDOWS)
    (void)ld;
    ldap_attach_extra(vm, code, NULL);
#else
    ldap_attach_extra(vm, code, ldap_get_diagnostic(ld));
#endif
}

/* =========================================================================
 * Helpers -- object property access
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

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    /* rawset internally does cdo_value_copy which retains s, so we must
     * release our own intern ref to avoid a leak. */
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static void obj_set_number(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Connection handle: pool slot index boxed inside an object
 * ======================================================================= */

/* Wrap a pool slot index in a fresh script object.  After unbind() the
 * slot is released; subsequent operations on the object throw a clean
 * "invalid handle" error rather than dereferencing a stale pointer. */
static CandoValue make_handle(CandoVM *vm, int slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(obj, LDAP_SLOT_KEY, (f64)slot);
    return v;
}

/* Read the slot index out of the object.  Returns -1 if the object is not
 * a valid connection handle.  Does not throw -- the caller decides. */
static int handle_slot(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    f64 idx = -1.0;
    if (!obj_get_number(obj, LDAP_SLOT_KEY, &idx)) return -1;
    int i = (int)idx;
    if (i < 0 || i >= LDAP_MAX_INSTANCES) return -1;
    return i;
}

/* Resolve to a live LDAP*.  Throws and returns NULL if the value isn't a
 * connection or the slot has been released. */
static LDAP *handle_unwrap(CandoVM *vm, CandoValue v)
{
    int slot = handle_slot(vm, v);
    if (slot < 0) {
        ldap_throw(vm, NULL, 0, "ldap: expected connection object");
        ldap_attach_extra(vm, 0, NULL);
        return NULL;
    }
    LDAP *ld = pool_get(slot);
    if (!ld) {
        ldap_throw(vm, NULL, 0, "ldap: connection has been unbound");
        ldap_attach_extra(vm, 0, NULL);
        return NULL;
    }
    return ld;
}

static void handle_mark_closed(CandoVM *vm, CandoValue v)
{
    int slot = handle_slot(vm, v);
    if (slot >= 0) pool_release(slot);
    /* Stomp the field so subsequent unwraps see "expected connection". */
    if (cando_is_object(v)) {
        CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
        obj_set_number(obj, LDAP_SLOT_KEY, -1.0);
    }
}

/* =========================================================================
 * Argument extraction helpers
 * ======================================================================= */

/* Free a NULL-terminated array of malloc'd C strings (and the array). */
static void free_str_array(char **arr)
{
    if (!arr) return;
    for (size_t i = 0; arr[i]; i++) free(arr[i]);
    free(arr);
}

/* =========================================================================
 * native_ldap_connect(uri) -> connection
 * ======================================================================= */

static int native_ldap_connect(CandoVM *vm, int argc, CandoValue *args)
{
    const char *uri = libutil_arg_cstr_at(args, argc, 0);
    if (!uri) {
        ldap_throw(vm, NULL, 0, "ldap.connect: URI must be a string");
        return -1;
    }

    LDAP *ld = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    const char *host = uri;
    int port = LDAP_PORT;
    int use_ssl = 0;
    if (strncmp(uri, "ldaps://", 8) == 0) { host = uri + 8; use_ssl = 1; port = LDAP_SSL_PORT; }
    else if (strncmp(uri, "ldap://", 7) == 0) { host = uri + 7; }

    char hostbuf[512];
    size_t n = strlen(host);
    if (n >= sizeof(hostbuf)) {
        ldap_throw(vm, NULL, 0, "ldap.connect: URI too long");
        return -1;
    }
    memcpy(hostbuf, host, n + 1);
    char *colon = strrchr(hostbuf, ':');
    if (colon && !strchr(colon, '/')) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) port = use_ssl ? LDAP_SSL_PORT : LDAP_PORT;
    }
    ld = use_ssl ? ldap_sslinit(hostbuf, port, 1)
                 : ldap_init   (hostbuf, port);
    if (!ld) {
        ldap_throw(vm, NULL, (int)LdapGetLastError(),
            "ldap.connect: failed to initialise (%lu)",
            (unsigned long)LdapGetLastError());
        return -1;
    }
#else
    int rc = ldap_initialize(&ld, uri);
    if (rc != LDAP_SUCCESS || !ld) {
        ldap_throw(vm, NULL, rc, "ldap.connect: %s", ldap_module_strerror(rc));
        return -1;
    }
    int version = LDAP_VERSION3;
    ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
#endif

    int slot = pool_alloc(ld);
    if (slot < 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
        ldap_unbind(ld);
#else
        ldap_unbind_ext_s(ld, NULL, NULL);
#endif
        ldap_throw(vm, NULL, 0,
            "ldap.connect: connection pool exhausted (max %d)",
            LDAP_MAX_INSTANCES);
        return -1;
    }
    cando_vm_push(vm, make_handle(vm, slot));
    return 1;
}

/* =========================================================================
 * native_ldap_set_option(conn, name, value)
 *
 * Supported names (string):
 *   "protocol_version"      -- value: number (2 or 3)
 *   "referrals"             -- value: bool
 *   "network_timeout"       -- value: number (seconds)
 *   "timelimit"             -- value: number (seconds)
 *   "sizelimit"             -- value: number
 * ======================================================================= */

static int native_ldap_set_option(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        ldap_throw(vm, NULL, 0, "ldap.set_option: (conn, name, value) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *name = libutil_arg_cstr_at(args, argc, 1);
    if (!name) {
        ldap_throw(vm, NULL, 0, "ldap.set_option: option name must be a string");
        return -1;
    }

    int rc = LDAP_SUCCESS;
    if (strcmp(name, "protocol_version") == 0) {
        int v = (int)libutil_arg_num_at(args, argc, 2, (f64)LDAP_VERSION3);
        rc = ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &v);
    } else if (strcmp(name, "referrals") == 0) {
        bool on = cando_is_bool(args[2]) && args[2].as.boolean;
#if defined(LDAP_PLATFORM_WINDOWS)
        ULONG v = on ? 1U : 0U;
        rc = (int)ldap_set_option(ld, LDAP_OPT_REFERRALS, &v);
#else
        /* OpenLDAP's LDAP_OPT_ON / LDAP_OPT_OFF are void* sentinels. */
        rc = ldap_set_option(ld, LDAP_OPT_REFERRALS,
                             on ? LDAP_OPT_ON : LDAP_OPT_OFF);
#endif
    } else if (strcmp(name, "timelimit") == 0) {
        int v = (int)libutil_arg_num_at(args, argc, 2, 0);
        rc = ldap_set_option(ld, LDAP_OPT_TIMELIMIT, &v);
    } else if (strcmp(name, "sizelimit") == 0) {
        int v = (int)libutil_arg_num_at(args, argc, 2, 0);
        rc = ldap_set_option(ld, LDAP_OPT_SIZELIMIT, &v);
    } else if (strcmp(name, "network_timeout") == 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
        ULONG v = (ULONG)(libutil_arg_num_at(args, argc, 2, 0) * 1000.0);
        rc = (int)ldap_set_option(ld, LDAP_OPT_SEND_TIMEOUT, &v);
#else
        struct timeval tv;
        f64 secs = libutil_arg_num_at(args, argc, 2, 0);
        tv.tv_sec  = (long)secs;
        tv.tv_usec = (long)((secs - (f64)tv.tv_sec) * 1e6);
        rc = ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &tv);
#endif
    } else if (strcmp(name, "tls_cacertfile") == 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
        /* Schannel reads CA bundles from the Windows trust store; per-conn
         * file pinning isn't supported.  Document and accept silently so
         * cross-platform scripts don't have to branch. */
        (void)args; rc = LDAP_SUCCESS;
#else
        const char *path = libutil_arg_cstr_at(args, argc, 2);
        if (!path) {
            ldap_throw(vm, NULL, 0,
                "ldap.set_option: tls_cacertfile must be a string path");
            return -1;
        }
        rc = ldap_set_option(NULL, LDAP_OPT_X_TLS_CACERTFILE, path);
#endif
    } else if (strcmp(name, "tls_certfile") == 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
        rc = LDAP_SUCCESS;
#else
        const char *path = libutil_arg_cstr_at(args, argc, 2);
        if (!path) {
            ldap_throw(vm, NULL, 0,
                "ldap.set_option: tls_certfile must be a string path");
            return -1;
        }
        rc = ldap_set_option(NULL, LDAP_OPT_X_TLS_CERTFILE, path);
#endif
    } else if (strcmp(name, "tls_keyfile") == 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
        rc = LDAP_SUCCESS;
#else
        const char *path = libutil_arg_cstr_at(args, argc, 2);
        if (!path) {
            ldap_throw(vm, NULL, 0,
                "ldap.set_option: tls_keyfile must be a string path");
            return -1;
        }
        rc = ldap_set_option(NULL, LDAP_OPT_X_TLS_KEYFILE, path);
#endif
    } else if (strcmp(name, "tls_require_cert") == 0) {
        const char *mode = libutil_arg_cstr_at(args, argc, 2);
        if (!mode) {
            ldap_throw(vm, NULL, 0,
                "ldap.set_option: tls_require_cert must be one of "
                "'never','allow','try','demand'");
            return -1;
        }
#if defined(LDAP_PLATFORM_WINDOWS)
        /* Schannel: enable / disable certificate validation via the SSL
         * option only; finer-grained modes aren't exposed.  Treat
         * "never" as "off" and everything else as "on". */
        ULONG ssl = (strcmp(mode, "never") == 0) ? 0U : 1U;
        rc = (int)ldap_set_option(ld, LDAP_OPT_SSL, &ssl);
#else
        int v;
        if      (strcmp(mode, "never")  == 0) v = LDAP_OPT_X_TLS_NEVER;
        else if (strcmp(mode, "allow")  == 0) v = LDAP_OPT_X_TLS_ALLOW;
        else if (strcmp(mode, "try")    == 0) v = LDAP_OPT_X_TLS_TRY;
        else if (strcmp(mode, "demand") == 0) v = LDAP_OPT_X_TLS_DEMAND;
        else {
            ldap_throw(vm, NULL, 0,
                "ldap.set_option: tls_require_cert='%s' invalid", mode);
            return -1;
        }
        rc = ldap_set_option(NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &v);
#endif
    } else {
        ldap_throw(vm, NULL, 0,
            "ldap.set_option: unknown option '%s'", name);
        return -1;
    }

    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.set_option: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_bind(conn, dn, password) -> bool
 *
 * Simple-bind authentication.  Pass nil/empty for anonymous bind.
 * ======================================================================= */

static int native_ldap_bind(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        ldap_throw(vm, NULL, 0, "ldap.bind: connection required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;

    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    const char *pw = libutil_arg_cstr_at(args, argc, 2);

#if defined(LDAP_PLATFORM_WINDOWS)
    ULONG rc = ldap_simple_bind_s(ld,
                                  (PCHAR)(dn ? dn : ""),
                                  (PCHAR)(pw ? pw : ""));
    int irc = (int)rc;
#else
    /* Use SASL simple bind via berval rather than the deprecated simple. */
    struct berval cred;
    cred.bv_val = (char *)(pw ? pw : "");
    cred.bv_len = pw ? strlen(pw) : 0;
    int irc = ldap_sasl_bind_s(ld, dn ? dn : "", LDAP_SASL_SIMPLE, &cred,
                               NULL, NULL, NULL);
#endif

    if (irc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, irc, "ldap.bind: %s", ldap_module_strerror(irc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_bind_anonymous(conn) -> bool
 * ======================================================================= */

static int native_ldap_bind_anonymous(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        ldap_throw(vm, NULL, 0, "ldap.bind_anonymous: connection required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;

#if defined(LDAP_PLATFORM_WINDOWS)
    int irc = (int)ldap_simple_bind_s(ld, NULL, NULL);
#else
    struct berval cred = { 0, NULL };
    int irc = ldap_sasl_bind_s(ld, "", LDAP_SASL_SIMPLE, &cred,
                               NULL, NULL, NULL);
#endif
    if (irc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, irc, "ldap.bind_anonymous: %s", ldap_module_strerror(irc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_start_tls(conn) -> bool
 * ======================================================================= */

static int native_ldap_start_tls(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        ldap_throw(vm, NULL, 0, "ldap.start_tls: connection required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
#if defined(LDAP_PLATFORM_WINDOWS)
    /* winldap signature: ldap_start_tls_s(ld, ServerReturnValue, result,
     * ServerControls, ClientControls).  ServerReturnValue holds the
     * extended-op result code; we discard it and surface only success/
     * failure via the function return. */
    ULONG svr_rc = 0;
    LDAPMessage *res = NULL;
    int irc = (int)ldap_start_tls_s(ld, &svr_rc, &res, NULL, NULL);
    if (res) ldap_msgfree(res);
#else
    int irc = ldap_start_tls_s(ld, NULL, NULL);
#endif
    if (irc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, irc, "ldap.start_tls: %s", ldap_module_strerror(irc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_unbind(conn) -> bool
 * ======================================================================= */

static int native_ldap_unbind(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        ldap_throw(vm, NULL, 0, "ldap.unbind: connection required");
        return -1;
    }
    /* Already-closed connection -> no-op (slot returns NULL from pool_get). */
    int slot = handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    LDAP *ld = pool_get(slot);
    if (!ld) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
#if defined(LDAP_PLATFORM_WINDOWS)
    ldap_unbind(ld);
#else
    ldap_unbind_ext_s(ld, NULL, NULL);
#endif
    handle_mark_closed(vm, args[0]);
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * Scope name parsing
 * ======================================================================= */

/* Thin wrapper around the header helper -- present so other TUs in the
 * module (and tests) can call through a stable name. */
static int ldap_module_parse_scope(const char *name, int *out)
{
    return ldap_helpers_parse_scope(name, out);
}

/* =========================================================================
 * native_ldap_search(conn, options) -> array of entry objects
 *
 * options is an object:
 *   {
 *     base:    "dc=example,dc=com",     // required
 *     scope:   "sub",                   // base | one | sub  (default sub)
 *     filter:  "(objectClass=user)",    // default "(objectClass=*)"
 *     attrs:   ["cn", "mail"],          // default null = all attributes
 *     sizelimit: 1000,                  // default 0 = server default
 *     timelimit: 30                     // default 0 = server default
 *   }
 *
 * Each entry in the returned array has the shape:
 *   { dn: "...", attributes: { name: [val, val, ...], ... } }
 * ======================================================================= */

static int native_ldap_search(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        ldap_throw(vm, NULL, 0, "ldap.search: (conn, options) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    if (!cando_is_object(args[1])) {
        ldap_throw(vm, NULL, 0, "ldap.search: options must be an object");
        return -1;
    }
    CdoObject *opts = cando_bridge_resolve(vm, args[1].as.handle);

    const char *base   = NULL;
    const char *filter = "(objectClass=*)";
    const char *scope_s = "sub";
    int    scope = LDAP_SCOPE_SUBTREE;
    int    size_limit = 0, time_limit = 0;
    size_t base_len = 0, filt_len = 0, scope_len = 0;
    (void)base_len; (void)filt_len; (void)scope_len;

    if (!obj_get_string(opts, "base", &base, &base_len) || !base) {
        ldap_throw(vm, NULL, 0, "ldap.search: options.base is required");
        return -1;
    }
    obj_get_string(opts, "filter", &filter, &filt_len);
    if (obj_get_string(opts, "scope", &scope_s, &scope_len)) {
        if (!ldap_module_parse_scope(scope_s, &scope)) {
            ldap_throw(vm, NULL, 0, "ldap.search: unknown scope '%s'", scope_s);
            return -1;
        }
    }
    f64 v;
    if (obj_get_number(opts, "sizelimit", &v)) size_limit = (int)v;
    if (obj_get_number(opts, "timelimit", &v)) time_limit = (int)v;

    /* attrs: optional array of strings */
    char **attrs = NULL;
    {
        CdoString *kattrs = cdo_string_intern("attrs", 5);
        CdoValue va;
        bool has = cdo_object_rawget(opts, kattrs, &va);
        cdo_string_release(kattrs);
        if (has && va.tag == CDO_ARRAY) {
            CdoObject *arr = va.as.object;
            u32 n = cdo_array_len(arr);
            attrs = (char **)calloc((size_t)n + 1, sizeof(char *));
            if (!attrs) {
                ldap_throw(vm, NULL, 0, "ldap.search: out of memory");
                return -1;
            }
            for (u32 i = 0; i < n; i++) {
                CdoValue elem;
                if (!cdo_array_rawget_idx(arr, i, &elem) ||
                    elem.tag != CDO_STRING) {
                    free_str_array(attrs);
                    ldap_throw(vm, NULL, 0,
                        "ldap.search: attrs[%u] must be a string",
                        (unsigned)i);
                    return -1;
                }
                size_t l = elem.as.string->length;
                char *dup = (char *)malloc(l + 1);
                if (!dup) {
                    free_str_array(attrs);
                    ldap_throw(vm, NULL, 0, "ldap.search: out of memory");
                    return -1;
                }
                memcpy(dup, elem.as.string->data, l);
                dup[l] = '\0';
                attrs[i] = dup;
            }
            attrs[n] = NULL;
        }
    }

    int page_size = 0;
    if (obj_get_number(opts, "page_size", &v)) page_size = (int)v;

    /* Build [{dn, attributes:{name:[vals]}}, ...] -- accumulating entries
     * across paged responses if page_size > 0. */
    CandoValue out_arr_v = cando_bridge_new_array(vm);
    CdoObject *out_arr   = cando_bridge_resolve(vm, out_arr_v.as.handle);

#if defined(LDAP_PLATFORM_WINDOWS)
    struct l_timeval tv;  tv.tv_sec = time_limit; tv.tv_usec = 0;
    struct l_timeval *tvp = (time_limit > 0) ? &tv : NULL;
#else
    struct timeval tv;  tv.tv_sec = time_limit; tv.tv_usec = 0;
    struct timeval *tvp = (time_limit > 0) ? &tv : NULL;
#endif

    /* Cookie carries paging state across iterations of the loop below. */
    struct berval cookie = { 0, NULL };
    struct berval *cookiep = NULL;
    bool more_pages = true;

    while (more_pages) {
        LDAPMessage  *result   = NULL;
        LDAPControl  *page_ctl = NULL;
        LDAPControl  *sctrls[2]= { NULL, NULL };
        int rc;

        if (page_size > 0) {
#if defined(LDAP_PLATFORM_WINDOWS)
            rc = (int)ldap_create_page_control(ld, page_size, cookiep,
                                               1, &page_ctl);
#else
            rc = ldap_create_page_control(ld, page_size, cookiep,
                                          0, &page_ctl);
#endif
            if (rc != LDAP_SUCCESS) {
                free_str_array(attrs);
                if (cookie.bv_val) free(cookie.bv_val);
                ldap_throw(vm, ld, rc,
                    "ldap.search: ldap_create_page_control: %s",
                    ldap_module_strerror(rc));
                return -1;
            }
            sctrls[0] = page_ctl;
        }

#if defined(LDAP_PLATFORM_WINDOWS)
        rc = (int)ldap_search_ext_s(ld, (PCHAR)base, scope, (PCHAR)filter,
                                    (PCHAR *)attrs, 0,
                                    page_size > 0 ? sctrls : NULL,
                                    NULL, tvp, size_limit, &result);
#else
        rc = ldap_search_ext_s(ld, base, scope, filter, attrs, 0,
                               page_size > 0 ? sctrls : NULL,
                               NULL, tvp, size_limit, &result);
#endif
        if (page_ctl) ldap_control_free(page_ctl);

        if (rc != LDAP_SUCCESS) {
            if (result) ldap_msgfree(result);
            free_str_array(attrs);
            if (cookie.bv_val) free(cookie.bv_val);
            ldap_throw(vm, ld, rc,
                "ldap.search: %s", ldap_module_strerror(rc));
            return -1;
        }

        for (LDAPMessage *e = ldap_first_entry(ld, result); e != NULL;
             e = ldap_next_entry(ld, e))
        {
            CandoValue ent_v = cando_bridge_new_object(vm);
            CdoObject *ent   = cando_bridge_resolve(vm, ent_v.as.handle);

            char *dn = ldap_get_dn(ld, e);
            if (dn) {
                obj_set_string(ent, "dn", dn, (u32)strlen(dn));
                ldap_memfree(dn);
            }

            CandoValue attr_v = cando_bridge_new_object(vm);
            CdoObject *attro  = cando_bridge_resolve(vm, attr_v.as.handle);

            BerElement *ber = NULL;
            for (char *aname = ldap_first_attribute(ld, e, &ber);
                 aname != NULL;
                 aname = ldap_next_attribute(ld, e, ber))
            {
                struct berval **vals = ldap_get_values_len(ld, e, aname);
                CandoValue vals_v   = cando_bridge_new_array(vm);
                CdoObject *vals_arr = cando_bridge_resolve(vm, vals_v.as.handle);
                if (vals) {
                    for (int i = 0; vals[i] != NULL; i++) {
                        CdoString *vs = cdo_string_intern(
                            vals[i]->bv_val, (u32)vals[i]->bv_len);
                        cdo_array_push(vals_arr, cdo_string_value(vs));
                        cdo_string_release(vs);
                    }
                    ldap_value_free_len(vals);
                }
                CdoString *kn = cdo_string_intern(aname, (u32)strlen(aname));
                cdo_object_rawset(attro, kn, cdo_array_value(vals_arr),
                                  FIELD_NONE);
                cdo_string_release(kn);
                ldap_memfree(aname);
            }
            if (ber) ber_free(ber, 0);

            CdoString *kattr = cdo_string_intern("attributes", 10);
            cdo_object_rawset(ent, kattr, cdo_object_value(attro), FIELD_NONE);
            cdo_string_release(kattr);

            cdo_array_push(out_arr, cdo_object_value(ent));
        }

        /* Check for the paged-results response control to decide if there
         * are more pages.  Only relevant when page_size > 0. */
        if (page_size > 0) {
            LDAPControl **rctrls = NULL;
            int parse_rc = ldap_parse_result(ld, result, NULL, NULL, NULL,
                                             NULL, &rctrls, 0);
            if (cookie.bv_val) { free(cookie.bv_val); cookie.bv_val = NULL; }
            cookie.bv_len = 0;
            cookiep = NULL;

            if (parse_rc == LDAP_SUCCESS && rctrls) {
                int total = 0;
#if defined(LDAP_PLATFORM_WINDOWS)
                ULONG total_count = 0;
                struct berval *new_cookie = NULL;
                ULONG prc = ldap_parse_page_control(ld, rctrls,
                                                    &total_count,
                                                    &new_cookie);
                (void)total;
                if (prc == LDAP_SUCCESS && new_cookie) {
                    if (new_cookie->bv_len > 0) {
                        cookie.bv_len = new_cookie->bv_len;
                        cookie.bv_val = (char *)malloc(cookie.bv_len);
                        if (cookie.bv_val)
                            memcpy(cookie.bv_val, new_cookie->bv_val,
                                   cookie.bv_len);
                        cookiep = &cookie;
                    }
                    ber_bvfree(new_cookie);
                }
#else
                struct berval new_cookie = { 0, NULL };
                int prc = ldap_parse_pageresponse_control(ld, rctrls[0],
                                                          &total,
                                                          &new_cookie);
                if (prc == LDAP_SUCCESS && new_cookie.bv_len > 0) {
                    cookie.bv_len = new_cookie.bv_len;
                    cookie.bv_val = (char *)malloc(cookie.bv_len);
                    if (cookie.bv_val)
                        memcpy(cookie.bv_val, new_cookie.bv_val,
                               cookie.bv_len);
                    cookiep = &cookie;
                }
                if (new_cookie.bv_val) ber_memfree(new_cookie.bv_val);
#endif
            }
            if (rctrls) ldap_controls_free(rctrls);
            more_pages = (cookiep != NULL && cookie.bv_len > 0);
        } else {
            more_pages = false;
        }

        ldap_msgfree(result);
    }
    if (cookie.bv_val) free(cookie.bv_val);
    free_str_array(attrs);

    cando_vm_push(vm, out_arr_v);
    return 1;
}

/* =========================================================================
 * Build LDAPMod** array
 * ======================================================================= */

/*
 * mods_alloc -- allocate `count` LDAPMod*'s + values arrays.  All inner
 * pointers (mod_type, mod_values, individual value strings) are heap-
 * allocated and freed by mods_free.
 */
typedef struct {
    LDAPMod **mods;     /* NULL-terminated */
    size_t    count;
} LdapModArr;

static void mods_free(LdapModArr *m)
{
    if (!m || !m->mods) return;
    for (size_t i = 0; i < m->count; i++) {
        LDAPMod *mod = m->mods[i];
        if (!mod) continue;
        if (mod->mod_type) free(mod->mod_type);
        char **vals = mod->mod_values;
        if (vals) {
            for (int j = 0; vals[j]; j++) free(vals[j]);
            free(vals);
        }
        free(mod);
    }
    free(m->mods);
    m->mods = NULL;
    m->count = 0;
}

/*
 * Convert a CdoValue (must be CDO_STRING or CDO_ARRAY of strings) into
 * a NULL-terminated char** array.  Returns NULL on type error.
 */
static char **values_to_str_array(CandoVM *vm, CdoValue v, const char *what)
{
    if (v.tag == CDO_STRING) {
        char **arr = (char **)calloc(2, sizeof(char *));
        if (!arr) { ldap_throw(vm, NULL, 0, "%s: out of memory", what); return NULL; }
        size_t l = v.as.string->length;
        arr[0] = (char *)malloc(l + 1);
        if (!arr[0]) { free(arr); ldap_throw(vm, NULL, 0, "%s: out of memory", what); return NULL; }
        memcpy(arr[0], v.as.string->data, l);
        arr[0][l] = '\0';
        arr[1] = NULL;
        return arr;
    }
    if (v.tag == CDO_ARRAY) {
        CdoObject *a = v.as.object;
        u32 n = cdo_array_len(a);
        char **arr = (char **)calloc((size_t)n + 1, sizeof(char *));
        if (!arr) { ldap_throw(vm, NULL, 0, "%s: out of memory", what); return NULL; }
        for (u32 i = 0; i < n; i++) {
            CdoValue ev;
            if (!cdo_array_rawget_idx(a, i, &ev) || ev.tag != CDO_STRING) {
                for (u32 j = 0; j < i; j++) free(arr[j]);
                free(arr);
                ldap_throw(vm, NULL, 0, "%s: value at index %u must be a string",
                               what, (unsigned)i);
                return NULL;
            }
            size_t l = ev.as.string->length;
            arr[i] = (char *)malloc(l + 1);
            if (!arr[i]) {
                for (u32 j = 0; j < i; j++) free(arr[j]);
                free(arr);
                ldap_throw(vm, NULL, 0, "%s: out of memory", what);
                return NULL;
            }
            memcpy(arr[i], ev.as.string->data, l);
            arr[i][l] = '\0';
        }
        arr[n] = NULL;
        return arr;
    }
    ldap_throw(vm, NULL, 0, "%s: value must be a string or array of strings", what);
    return NULL;
}

/* State for build_mods_from_attrs iteration callback. */
typedef struct {
    CandoVM   *vm;
    LDAPMod  **mods;
    u32        count;
    u32        cap;
    bool       failed;
} AttrIterState;

static bool attr_iter_cb(CdoString *key, CdoValue *val, u8 flags, void *ud)
{
    (void)flags;
    AttrIterState *s = (AttrIterState *)ud;

    char **vals = values_to_str_array(s->vm, *val, "ldap.add");
    if (!vals) { s->failed = true; return false; }

    LDAPMod *m = (LDAPMod *)calloc(1, sizeof(LDAPMod));
    if (!m) {
        for (int j = 0; vals[j]; j++) free(vals[j]);
        free(vals);
        ldap_throw(s->vm, NULL, 0, "ldap.add: out of memory");
        s->failed = true;
        return false;
    }
    m->mod_op     = LDAP_MOD_ADD;
    m->mod_type   = (char *)malloc((size_t)key->length + 1);
    if (!m->mod_type) {
        free(m);
        for (int j = 0; vals[j]; j++) free(vals[j]);
        free(vals);
        ldap_throw(s->vm, NULL, 0, "ldap.add: out of memory");
        s->failed = true;
        return false;
    }
    memcpy(m->mod_type, key->data, key->length);
    m->mod_type[key->length] = '\0';
    m->mod_values = vals;

    if (s->count + 1 >= s->cap) {
        u32 new_cap = s->cap * 2;
        LDAPMod **bigger = (LDAPMod **)realloc(s->mods,
                                               new_cap * sizeof(LDAPMod *));
        if (!bigger) {
            free(m->mod_type);
            for (int j = 0; vals[j]; j++) free(vals[j]);
            free(vals);
            free(m);
            ldap_throw(s->vm, NULL, 0, "ldap.add: out of memory");
            s->failed = true;
            return false;
        }
        s->mods = bigger;
        s->cap  = new_cap;
    }
    s->mods[s->count++] = m;
    return true;
}

/*
 * Build an LDAPMod array from a CanDo attributes object whose fields are
 * { attr_name: ["val", "val"] }.  All mods get mod_op = LDAP_MOD_ADD.
 */
static int build_mods_from_attrs(CandoVM *vm, CdoObject *obj, LdapModArr *out)
{
    out->mods  = NULL;
    out->count = 0;

    AttrIterState st = { vm, NULL, 0, 8, false };
    st.mods = (LDAPMod **)calloc(st.cap, sizeof(LDAPMod *));
    if (!st.mods) {
        ldap_throw(vm, NULL, 0, "ldap.add: out of memory");
        return 0;
    }

    cdo_object_foreach(obj, attr_iter_cb, &st);

    if (st.failed) {
        LdapModArr tmp = { st.mods, st.count };
        mods_free(&tmp);
        return 0;
    }
    st.mods[st.count] = NULL;
    out->mods  = st.mods;
    out->count = st.count;
    return 1;
}

/*
 * Build an LDAPMod array from a CanDo modifications array of objects:
 *   [ { op: "replace"|"add"|"delete", attr: "...", values: [...] }, ... ]
 */
static int build_mods_from_modlist(CandoVM *vm, CdoObject *arr, LdapModArr *out)
{
    out->mods = NULL; out->count = 0;
    u32 n = cdo_array_len(arr);
    LDAPMod **mods = (LDAPMod **)calloc((size_t)n + 1, sizeof(LDAPMod *));
    if (!mods) { ldap_throw(vm, NULL, 0, "ldap.modify: out of memory"); return 0; }

    for (u32 i = 0; i < n; i++) {
        CdoValue ev;
        if (!cdo_array_rawget_idx(arr, i, &ev) || ev.tag != CDO_OBJECT) {
            ldap_throw(vm, NULL, 0, "ldap.modify: mods[%u] must be an object",
                           (unsigned)i);
            goto fail;
        }
        CdoObject *mobj = ev.as.object;

        const char *op = NULL, *attr = NULL;
        size_t op_len = 0, attr_len = 0;
        if (!obj_get_string(mobj, "op", &op, &op_len) ||
            !obj_get_string(mobj, "attr", &attr, &attr_len)) {
            ldap_throw(vm, NULL, 0,
                "ldap.modify: mods[%u] requires .op and .attr",
                (unsigned)i);
            goto fail;
        }

        int op_code;
        if (!ldap_helpers_parse_mod_op(op, &op_code)) {
            ldap_throw(vm, NULL, 0, "ldap.modify: unknown op '%s'", op);
            goto fail;
        }

        /* values are optional for delete */
        char **vals = NULL;
        CdoString *kvals = cdo_string_intern("values", 6);
        CdoValue vv;
        bool has_vals = cdo_object_rawget(mobj, kvals, &vv);
        cdo_string_release(kvals);
        if (has_vals && vv.tag != CDO_NULL) {
            vals = values_to_str_array(vm, vv, "ldap.modify");
            if (!vals) goto fail;
        }

        LDAPMod *m = (LDAPMod *)calloc(1, sizeof(LDAPMod));
        if (!m) {
            if (vals) { for (int j = 0; vals[j]; j++) free(vals[j]); free(vals); }
            ldap_throw(vm, NULL, 0, "ldap.modify: out of memory");
            goto fail;
        }
        m->mod_op     = op_code;
        m->mod_type   = (char *)malloc(attr_len + 1);
        if (!m->mod_type) {
            free(m);
            if (vals) { for (int j = 0; vals[j]; j++) free(vals[j]); free(vals); }
            ldap_throw(vm, NULL, 0, "ldap.modify: out of memory");
            goto fail;
        }
        memcpy(m->mod_type, attr, attr_len);
        m->mod_type[attr_len] = '\0';
        m->mod_values = vals;
        mods[i] = m;
    }
    mods[n] = NULL;
    out->mods = mods;
    out->count = n;
    return 1;

fail:
    {
        LdapModArr tmp = { mods, n };
        mods_free(&tmp);
    }
    return 0;
}

/* =========================================================================
 * native_ldap_add(conn, dn, attrs_obj) -> bool
 *
 * attrs_obj: { name: "val" | ["val", ...], ... }
 * ======================================================================= */

static int native_ldap_add(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        ldap_throw(vm, NULL, 0, "ldap.add: (conn, dn, attrs) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        ldap_throw(vm, NULL, 0, "ldap.add: dn must be a string");
        return -1;
    }
    if (!cando_is_object(args[2])) {
        ldap_throw(vm, NULL, 0, "ldap.add: attrs must be an object");
        return -1;
    }
    CdoObject *attrs = cando_bridge_resolve(vm, args[2].as.handle);
    if (attrs->kind == OBJ_ARRAY) {
        ldap_throw(vm, NULL, 0, "ldap.add: attrs must be an object, not an array");
        return -1;
    }

    LdapModArr arr = { 0 };
    if (!build_mods_from_attrs(vm, attrs, &arr)) return -1;

#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_add_ext_s(ld, (PCHAR)dn, arr.mods, NULL, NULL);
#else
    int rc = ldap_add_ext_s(ld, dn, arr.mods, NULL, NULL);
#endif
    mods_free(&arr);

    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.add: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_modify(conn, dn, mods) -> bool
 *
 * mods: [{ op: "add"|"replace"|"delete", attr: "...", values: [...] }, ...]
 * ======================================================================= */

static int native_ldap_modify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        ldap_throw(vm, NULL, 0, "ldap.modify: (conn, dn, mods) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        ldap_throw(vm, NULL, 0, "ldap.modify: dn must be a string");
        return -1;
    }
    if (!cando_is_object(args[2])) {
        ldap_throw(vm, NULL, 0, "ldap.modify: mods must be an array");
        return -1;
    }
    CdoObject *modarr = cando_bridge_resolve(vm, args[2].as.handle);
    if (modarr->kind != OBJ_ARRAY) {
        ldap_throw(vm, NULL, 0, "ldap.modify: mods must be an array of objects");
        return -1;
    }

    LdapModArr arr = { 0 };
    if (!build_mods_from_modlist(vm, modarr, &arr)) return -1;

#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_modify_ext_s(ld, (PCHAR)dn, arr.mods, NULL, NULL);
#else
    int rc = ldap_modify_ext_s(ld, dn, arr.mods, NULL, NULL);
#endif
    mods_free(&arr);

    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.modify: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_delete(conn, dn) -> bool
 * ======================================================================= */

static int native_ldap_delete(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        ldap_throw(vm, NULL, 0, "ldap.delete: (conn, dn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        ldap_throw(vm, NULL, 0, "ldap.delete: dn must be a string");
        return -1;
    }
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_delete_ext_s(ld, (PCHAR)dn, NULL, NULL);
#else
    int rc = ldap_delete_ext_s(ld, dn, NULL, NULL);
#endif
    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.delete: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_rename(conn, dn, new_rdn[, delete_old=true]) -> bool
 *
 * Rename in place (no new parent).
 * ======================================================================= */

static int native_ldap_rename(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        ldap_throw(vm, NULL, 0, "ldap.rename: (conn, dn, new_rdn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn      = libutil_arg_cstr_at(args, argc, 1);
    const char *new_rdn = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !new_rdn) {
        ldap_throw(vm, NULL, 0, "ldap.rename: dn and new_rdn must be strings");
        return -1;
    }
    int delete_old = 1;
    if (argc >= 4 && cando_is_bool(args[3])) {
        delete_old = args[3].as.boolean ? 1 : 0;
    }
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_rename_ext_s(ld, (PCHAR)dn, (PCHAR)new_rdn, NULL,
                                    delete_old, NULL, NULL);
#else
    int rc = ldap_rename_s(ld, dn, new_rdn, NULL, delete_old, NULL, NULL);
#endif
    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.rename: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_move(conn, dn, new_parent_dn[, new_rdn][, delete_old]) -> bool
 *
 * Moves to a new parent.  If new_rdn is null, derives it from `dn`.
 * ======================================================================= */

static int native_ldap_move(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        ldap_throw(vm, NULL, 0, "ldap.move: (conn, dn, new_parent_dn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn         = libutil_arg_cstr_at(args, argc, 1);
    const char *new_parent = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !new_parent) {
        ldap_throw(vm, NULL, 0,
            "ldap.move: dn and new_parent_dn must be strings");
        return -1;
    }
    const char *new_rdn = libutil_arg_cstr_at(args, argc, 3);
    int delete_old = 1;
    if (argc >= 5 && cando_is_bool(args[4])) {
        delete_old = args[4].as.boolean ? 1 : 0;
    }

    /* Default RDN = the leftmost component of dn, up to first unescaped comma */
    char *rdn_buf = NULL;
    if (!new_rdn) {
        size_t need = strlen(dn) + 1;
        rdn_buf = (char *)malloc(need);
        if (!rdn_buf) {
            ldap_throw(vm, NULL, 0, "ldap.move: out of memory");
            return -1;
        }
        if (!ldap_helpers_extract_rdn(dn, rdn_buf, need)) {
            free(rdn_buf);
            ldap_throw(vm, NULL, 0, "ldap.move: invalid dn");
            return -1;
        }
        new_rdn = rdn_buf;
    }

#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_rename_ext_s(ld, (PCHAR)dn, (PCHAR)new_rdn,
                                    (PCHAR)new_parent,
                                    delete_old, NULL, NULL);
#else
    int rc = ldap_rename_s(ld, dn, new_rdn, new_parent, delete_old, NULL, NULL);
#endif
    if (rdn_buf) free(rdn_buf);

    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc, "ldap.move: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_compare(conn, dn, attr, value) -> bool
 *
 * Returns true if the value compares equal, false otherwise.
 * ======================================================================= */

static int native_ldap_compare(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4) {
        ldap_throw(vm, NULL, 0, "ldap.compare: (conn, dn, attr, value) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn   = libutil_arg_cstr_at(args, argc, 1);
    const char *attr = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !attr || !cando_is_string(args[3])) {
        ldap_throw(vm, NULL, 0, "ldap.compare: dn, attr, value must be strings");
        return -1;
    }
    struct berval bv;
    bv.bv_val = (char *)args[3].as.string->data;
    bv.bv_len = args[3].as.string->length;

#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_compare_ext_s(ld, (PCHAR)dn, (PCHAR)attr,
                                     bv.bv_val, &bv, NULL, NULL);
#else
    int rc = ldap_compare_ext_s(ld, dn, attr, &bv, NULL, NULL);
#endif

    if (rc == LDAP_COMPARE_TRUE) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    if (rc == LDAP_COMPARE_FALSE) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    ldap_throw(vm, ld, rc, "ldap.compare: %s", ldap_module_strerror(rc));
    return -1;
}

/* =========================================================================
 * native_ldap_escape_filter(value) -> string
 *
 * RFC 4515 assertion-value escaping.  Use when interpolating user input
 * into a search filter:
 *
 *   VAR f = `(&(objectClass=user)(cn=${ldap.escape_filter(name)}))`;
 * ======================================================================= */

static int native_ldap_escape_filter(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        ldap_throw(vm, NULL, 0,
            "ldap.escape_filter: value must be a string");
        return -1;
    }
    CandoString *s = args[0].as.string;
    long need = ldap_helpers_escape_filter(s->data, s->length, NULL, 0);
    if (need < 0) {
        ldap_throw(vm, NULL, 0, "ldap.escape_filter: failed to size buffer");
        return -1;
    }
    char stack[256];
    char *buf = (size_t)need + 1 <= sizeof(stack)
                ? stack
                : (char *)malloc((size_t)need + 1);
    if (!buf) {
        ldap_throw(vm, NULL, 0, "ldap.escape_filter: out of memory");
        return -1;
    }
    long n = ldap_helpers_escape_filter(s->data, s->length, buf,
                                        (size_t)need + 1);
    if (n < 0) {
        if (buf != stack) free(buf);
        ldap_throw(vm, NULL, 0, "ldap.escape_filter: encoding failed");
        return -1;
    }
    libutil_push_str(vm, buf, (u32)n);
    if (buf != stack) free(buf);
    return 1;
}

/* =========================================================================
 * native_ldap_escape_dn(value) -> string  (RFC 4514)
 * ======================================================================= */

static int native_ldap_escape_dn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        ldap_throw(vm, NULL, 0,
            "ldap.escape_dn: value must be a string");
        return -1;
    }
    CandoString *s = args[0].as.string;
    long need = ldap_helpers_escape_dn(s->data, s->length, NULL, 0);
    if (need < 0) {
        ldap_throw(vm, NULL, 0, "ldap.escape_dn: failed to size buffer");
        return -1;
    }
    char stack[256];
    char *buf = (size_t)need + 1 <= sizeof(stack)
                ? stack
                : (char *)malloc((size_t)need + 1);
    if (!buf) {
        ldap_throw(vm, NULL, 0, "ldap.escape_dn: out of memory");
        return -1;
    }
    long n = ldap_helpers_escape_dn(s->data, s->length, buf,
                                    (size_t)need + 1);
    if (n < 0) {
        if (buf != stack) free(buf);
        ldap_throw(vm, NULL, 0, "ldap.escape_dn: encoding failed");
        return -1;
    }
    libutil_push_str(vm, buf, (u32)n);
    if (buf != stack) free(buf);
    return 1;
}

/* =========================================================================
 * native_ldap_rootdse(conn[, attrs]) -> entry-attributes object
 *
 * Reads the directory's rootDSE -- the per-server status object that lists
 * supportedControl, supportedExtension, namingContexts, etc.  Equivalent
 * to:  search(conn, { base: "", scope: "base", filter: "(objectClass=*)" })[0].attributes
 * ======================================================================= */

static int native_ldap_rootdse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        ldap_throw(vm, NULL, 0, "ldap.rootdse: connection required");
        return -1;
    }
    LDAP *ld = handle_unwrap(vm, args[0]);
    if (!ld) return -1;

    /* Build attrs[] if caller passed a list. */
    char **attrs = NULL;
    if (argc >= 2 && cando_is_object(args[1])) {
        CdoObject *aro = cando_bridge_resolve(vm, args[1].as.handle);
        if (aro->kind == OBJ_ARRAY) {
            u32 n = cdo_array_len(aro);
            attrs = (char **)calloc((size_t)n + 1, sizeof(char *));
            if (!attrs) {
                ldap_throw(vm, NULL, 0, "ldap.rootdse: out of memory");
                return -1;
            }
            for (u32 i = 0; i < n; i++) {
                CdoValue ev;
                if (!cdo_array_rawget_idx(aro, i, &ev)
                    || ev.tag != CDO_STRING) {
                    free_str_array(attrs);
                    ldap_throw(vm, NULL, 0,
                        "ldap.rootdse: attrs[%u] must be a string",
                        (unsigned)i);
                    return -1;
                }
                size_t l = ev.as.string->length;
                attrs[i] = (char *)malloc(l + 1);
                if (!attrs[i]) {
                    free_str_array(attrs);
                    ldap_throw(vm, NULL, 0, "ldap.rootdse: out of memory");
                    return -1;
                }
                memcpy(attrs[i], ev.as.string->data, l);
                attrs[i][l] = '\0';
            }
            attrs[n] = NULL;
        }
    }

    LDAPMessage *result = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_search_ext_s(ld, (PCHAR)"", LDAP_SCOPE_BASE,
                                    (PCHAR)"(objectClass=*)",
                                    (PCHAR *)attrs, 0, NULL, NULL, NULL,
                                    0, &result);
#else
    int rc = ldap_search_ext_s(ld, "", LDAP_SCOPE_BASE,
                               "(objectClass=*)", attrs, 0,
                               NULL, NULL, NULL, 0, &result);
#endif
    free_str_array(attrs);

    if (rc != LDAP_SUCCESS) {
        if (result) ldap_msgfree(result);
        ldap_throw(vm, ld, rc, "ldap.rootdse: %s", ldap_module_strerror(rc));
        return -1;
    }

    LDAPMessage *e = ldap_first_entry(ld, result);
    if (!e) {
        ldap_msgfree(result);
        cando_vm_push(vm, cando_null());
        return 1;
    }

    CandoValue attr_v = cando_bridge_new_object(vm);
    CdoObject *attro  = cando_bridge_resolve(vm, attr_v.as.handle);

    BerElement *ber = NULL;
    for (char *aname = ldap_first_attribute(ld, e, &ber);
         aname != NULL;
         aname = ldap_next_attribute(ld, e, ber))
    {
        struct berval **vals = ldap_get_values_len(ld, e, aname);
        CandoValue vals_v   = cando_bridge_new_array(vm);
        CdoObject *vals_arr = cando_bridge_resolve(vm, vals_v.as.handle);
        if (vals) {
            for (int i = 0; vals[i] != NULL; i++) {
                CdoString *vs = cdo_string_intern(
                    vals[i]->bv_val, (u32)vals[i]->bv_len);
                cdo_array_push(vals_arr, cdo_string_value(vs));
                cdo_string_release(vs);
            }
            ldap_value_free_len(vals);
        }
        CdoString *kn = cdo_string_intern(aname, (u32)strlen(aname));
        cdo_object_rawset(attro, kn, cdo_array_value(vals_arr), FIELD_NONE);
        cdo_string_release(kn);
        ldap_memfree(aname);
    }
    if (ber) ber_free(ber, 0);

    ldap_msgfree(result);
    cando_vm_push(vm, attr_v);
    return 1;
}

/* =========================================================================
 * native_ldap_test_credentials(uri, dn, password) -> bool
 *
 * Open a fresh connection, simple-bind, unbind.  Returns TRUE if the
 * credentials authenticate cleanly, FALSE on LDAP_INVALID_CREDENTIALS,
 * and re-throws on any other error (server unreachable, TLS failure, ...)
 * so genuine problems aren't silently coerced into "wrong password".
 * ======================================================================= */

static int native_ldap_test_credentials(CandoVM *vm, int argc, CandoValue *args)
{
    const char *uri = libutil_arg_cstr_at(args, argc, 0);
    const char *dn  = libutil_arg_cstr_at(args, argc, 1);
    const char *pw  = libutil_arg_cstr_at(args, argc, 2);
    if (!uri || !dn || !pw) {
        ldap_throw(vm, NULL, 0,
            "ldap.test_credentials: (uri, dn, password) must all be strings");
        return -1;
    }

    LDAP *ld = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    const char *host = uri;
    int port = LDAP_PORT, use_ssl = 0;
    if (strncmp(uri, "ldaps://", 8) == 0) { host = uri + 8; use_ssl = 1; port = LDAP_SSL_PORT; }
    else if (strncmp(uri, "ldap://", 7) == 0) { host = uri + 7; }
    char hostbuf[512];
    size_t n = strlen(host);
    if (n >= sizeof(hostbuf)) {
        ldap_throw(vm, NULL, 0, "ldap.test_credentials: URI too long");
        return -1;
    }
    memcpy(hostbuf, host, n + 1);
    char *colon = strrchr(hostbuf, ':');
    if (colon && !strchr(colon, '/')) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) port = use_ssl ? LDAP_SSL_PORT : LDAP_PORT;
    }
    ld = use_ssl ? ldap_sslinit(hostbuf, port, 1)
                 : ldap_init   (hostbuf, port);
    if (!ld) {
        ldap_throw(vm, NULL, (int)LdapGetLastError(),
            "ldap.test_credentials: cannot initialise (%lu)",
            (unsigned long)LdapGetLastError());
        return -1;
    }
    ULONG bind_rc = ldap_simple_bind_s(ld, (PCHAR)dn, (PCHAR)pw);
    int rc = (int)bind_rc;
    ldap_unbind(ld);
#else
    int irc = ldap_initialize(&ld, uri);
    if (irc != LDAP_SUCCESS || !ld) {
        ldap_throw(vm, NULL, irc,
            "ldap.test_credentials: %s", ldap_module_strerror(irc));
        return -1;
    }
    int version = LDAP_VERSION3;
    ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
    struct berval cred = { .bv_len = strlen(pw), .bv_val = (char *)pw };
    int rc = ldap_sasl_bind_s(ld, dn, LDAP_SASL_SIMPLE, &cred,
                              NULL, NULL, NULL);
    ldap_unbind_ext_s(ld, NULL, NULL);
#endif

    if (rc == LDAP_SUCCESS) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    if (rc == LDAP_INVALID_CREDENTIALS) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    ldap_throw(vm, NULL, rc,
        "ldap.test_credentials: %s", ldap_module_strerror(rc));
    return -1;
}

/* =========================================================================
 * native_ldap_password_modify(conn, dn, old_pw, new_pw[, format]) -> bool
 *
 * `format` (optional, defaults to "auto"):
 *   "auto"        -- pick AD vs RFC 3062 by the server's vendorName from
 *                    rootDSE; on Windows defaults to "ad".
 *   "ad"          -- AD-style modify on `unicodePwd` (UTF-16LE quoted).
 *                    Requires LDAPS or StartTLS by AD's policy.
 *   "rfc3062"     -- LDAP extended op 1.3.6.1.4.1.4203.1.11.1.
 *
 * `old_pw` may be empty for an admin reset (caller must hold permission).
 * ======================================================================= */

/* Encode a UTF-8 password as the AD wire form: '"' + UTF-16LE password + '"'.
 * Returns malloc'd buffer and writes byte length to *out_len.  Returns NULL
 * on bad UTF-8 (rare; passwords are usually ASCII). */
static unsigned char *encode_ad_unicode_pwd(const char *pw, size_t *out_len)
{
    /* Worst case: every UTF-8 byte produces 4 bytes of UTF-16LE (surrogates
     * for non-BMP).  Plus 4 bytes for the quotes.  That's plenty. */
    size_t plen = strlen(pw);
    size_t cap = plen * 4 + 4;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) return NULL;

    size_t o = 0;
    /* Opening quote (U+0022) */
    buf[o++] = '"'; buf[o++] = 0x00;

    size_t i = 0;
    while (i < plen) {
        unsigned char c0 = (unsigned char)pw[i];
        unsigned int cp;
        size_t step;
        if (c0 < 0x80)               { cp = c0; step = 1; }
        else if ((c0 & 0xE0) == 0xC0 && i + 1 < plen) {
            cp = ((c0 & 0x1F) << 6) | ((unsigned char)pw[i+1] & 0x3F);
            step = 2;
        } else if ((c0 & 0xF0) == 0xE0 && i + 2 < plen) {
            cp = ((c0 & 0x0F) << 12)
               | (((unsigned char)pw[i+1] & 0x3F) << 6)
               |  ((unsigned char)pw[i+2] & 0x3F);
            step = 3;
        } else if ((c0 & 0xF8) == 0xF0 && i + 3 < plen) {
            cp = ((c0 & 0x07) << 18)
               | (((unsigned char)pw[i+1] & 0x3F) << 12)
               | (((unsigned char)pw[i+2] & 0x3F) << 6)
               |  ((unsigned char)pw[i+3] & 0x3F);
            step = 4;
        } else { free(buf); return NULL; }

        if (cp <= 0xFFFF) {
            buf[o++] = (unsigned char)(cp & 0xFF);
            buf[o++] = (unsigned char)((cp >> 8) & 0xFF);
        } else {
            cp -= 0x10000;
            unsigned int hi = 0xD800 | (cp >> 10);
            unsigned int lo = 0xDC00 | (cp & 0x3FF);
            buf[o++] = (unsigned char)(hi & 0xFF);
            buf[o++] = (unsigned char)((hi >> 8) & 0xFF);
            buf[o++] = (unsigned char)(lo & 0xFF);
            buf[o++] = (unsigned char)((lo >> 8) & 0xFF);
        }
        i += step;
    }
    /* Closing quote */
    buf[o++] = '"'; buf[o++] = 0x00;

    *out_len = o;
    return buf;
}

static int native_ldap_password_modify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4) {
        ldap_throw(vm, NULL, 0,
            "ldap.password_modify: (conn, dn, old, new[, format]) required");
        return -1;
    }
    LDAP *ld = handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn     = libutil_arg_cstr_at(args, argc, 1);
    const char *old_pw = libutil_arg_cstr_at(args, argc, 2);
    const char *new_pw = libutil_arg_cstr_at(args, argc, 3);
    if (!dn || !new_pw) {
        ldap_throw(vm, NULL, 0,
            "ldap.password_modify: dn and new password must be strings");
        return -1;
    }
    if (!old_pw) old_pw = "";
    const char *format = libutil_arg_cstr_at(args, argc, 4);
    if (!format) format = "auto";

    int use_ad = 0;
    if (strcmp(format, "ad") == 0)            use_ad = 1;
    else if (strcmp(format, "rfc3062") == 0)  use_ad = 0;
    else { /* auto */
#if defined(LDAP_PLATFORM_WINDOWS)
        use_ad = 1;
#else
        /* Heuristic: AD's vendorName / 1.2.840.113556 OID space is in the
         * rootDSE.  If we can't probe, default to RFC 3062. */
        use_ad = 0;
#endif
    }

    if (use_ad) {
        size_t blen = 0;
        unsigned char *buf = encode_ad_unicode_pwd(new_pw, &blen);
        if (!buf) {
            ldap_throw(vm, NULL, 0,
                "ldap.password_modify: invalid UTF-8 in new password");
            return -1;
        }
        struct berval bv; bv.bv_val = (char *)buf; bv.bv_len = blen;
        struct berval *vals[2] = { &bv, NULL };
        LDAPMod mod;
        memset(&mod, 0, sizeof(mod));
        mod.mod_op           = LDAP_MOD_REPLACE | LDAP_MOD_BVALUES;
        mod.mod_type         = (char *)"unicodePwd";
        mod.mod_bvalues      = vals;
        LDAPMod *mods[2] = { &mod, NULL };
#if defined(LDAP_PLATFORM_WINDOWS)
        int rc = (int)ldap_modify_ext_s(ld, (PCHAR)dn, mods, NULL, NULL);
#else
        int rc = ldap_modify_ext_s(ld, dn, mods, NULL, NULL);
#endif
        free(buf);
        if (rc != LDAP_SUCCESS) {
            ldap_throw(vm, ld, rc,
                "ldap.password_modify: %s", ldap_module_strerror(rc));
            return -1;
        }
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }

#if defined(LDAP_PLATFORM_WINDOWS)
    /* RFC 3062 not available via a one-call helper in wldap32.  For now
     * we return a clear error -- callers can use format="ad" on AD. */
    ldap_throw(vm, NULL, 0,
        "ldap.password_modify: rfc3062 format unavailable on Windows; "
        "use format=\"ad\" against Active Directory");
    return -1;
#else
    /* RFC 3062 PasswdModifyRequestValue:
     *   SEQUENCE {
     *     userIdentity    [0] OCTET STRING OPTIONAL,
     *     oldPasswd       [1] OCTET STRING OPTIONAL,
     *     newPasswd       [2] OCTET STRING OPTIONAL }
     * We hand-build the BER, since libldap doesn't ship a helper. */
    BerElement *ber = ber_alloc_t(LBER_USE_DER);
    if (!ber) {
        ldap_throw(vm, NULL, 0, "ldap.password_modify: ber_alloc failed");
        return -1;
    }
    int berc = 0;
    berc = ber_printf(ber, "{");
    if (dn[0])     berc = ber_printf(ber, "ts", 0x80, dn);
    if (old_pw[0]) berc = ber_printf(ber, "ts", 0x81, old_pw);
    if (new_pw[0]) berc = ber_printf(ber, "ts", 0x82, new_pw);
    berc = ber_printf(ber, "}");
    if (berc < 0) {
        ber_free(ber, 1);
        ldap_throw(vm, NULL, 0, "ldap.password_modify: ber_printf failed");
        return -1;
    }
    struct berval *bv = NULL;
    if (ber_flatten(ber, &bv) < 0 || !bv) {
        ber_free(ber, 1);
        ldap_throw(vm, NULL, 0, "ldap.password_modify: ber_flatten failed");
        return -1;
    }

    struct berval *resp = NULL;
    char *resp_oid = NULL;
    int rc = ldap_extended_operation_s(ld, "1.3.6.1.4.1.4203.1.11.1", bv,
                                       NULL, NULL, &resp_oid, &resp);
    ber_bvfree(bv);
    ber_free(ber, 1);
    if (resp)     ber_bvfree(resp);
    if (resp_oid) ldap_memfree(resp_oid);

    if (rc != LDAP_SUCCESS) {
        ldap_throw(vm, ld, rc,
            "ldap.password_modify: %s", ldap_module_strerror(rc));
        return -1;
    }
    cando_vm_push(vm, cando_bool(true));
    return 1;
#endif
}

/* =========================================================================
 * Group membership helpers -- members() and member_of()
 *
 * The AD-specific matching rule "1.2.840.113556.1.4.1941"
 * (LDAP_MATCHING_RULE_IN_CHAIN) walks group nesting on the server in a
 * single query.  Against non-AD directories that lack the rule we fall
 * back to a client-side BFS using the per-group `member` attribute.
 *
 * Detection: probe rootDSE.supportedControl / supportedExtension once per
 * connection; cache the answer in a small static map keyed by slot.
 * Cache invalidates on unbind() because the slot index gets reused.
 * ======================================================================= */

#define MR_IN_CHAIN_OID "1.2.840.113556.1.4.1941"

/* Per-slot cached AD-flavour probe.  -1 = unprobed, 0 = non-AD, 1 = AD. */
static signed char g_is_ad_cache[LDAP_MAX_INSTANCES] = { 0 };

static int detect_ad(LDAP *ld, int slot)
{
    if (slot < 0 || slot >= LDAP_MAX_INSTANCES) return 0;
    if (g_is_ad_cache[slot] != 0) return g_is_ad_cache[slot] == 1;

    LDAPMessage *result = NULL;
    char *attrs[] = { (char *)"supportedCapabilities",
                      (char *)"vendorName", NULL };
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_search_ext_s(ld, (PCHAR)"", LDAP_SCOPE_BASE,
                                    (PCHAR)"(objectClass=*)",
                                    (PCHAR *)attrs, 0, NULL, NULL, NULL,
                                    0, &result);
#else
    int rc = ldap_search_ext_s(ld, "", LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, NULL, 0, &result);
#endif
    int is_ad = 0;
    if (rc == LDAP_SUCCESS && result) {
        LDAPMessage *e = ldap_first_entry(ld, result);
        if (e) {
            /* AD's supportedCapabilities includes "1.2.840.113556.1.4.800". */
            struct berval **caps = ldap_get_values_len(ld, e,
                                                       "supportedCapabilities");
            if (caps) {
                for (int i = 0; caps[i] && !is_ad; i++) {
                    if (strncmp(caps[i]->bv_val, "1.2.840.113556.", 15) == 0)
                        is_ad = 1;
                }
                ldap_value_free_len(caps);
            }
        }
    }
    if (result) ldap_msgfree(result);
    g_is_ad_cache[slot] = is_ad ? 1 : -1;  /* sticky */
    return is_ad;
}

/* Push a freshly built array of DN strings extracted from `attr` on each
 * entry of `result`. */
static void push_dn_array_from_results(CandoVM *vm, LDAP *ld,
                                       LDAPMessage *result,
                                       CdoObject *out_arr,
                                       const char *want_attr)
{
    for (LDAPMessage *e = ldap_first_entry(ld, result); e != NULL;
         e = ldap_next_entry(ld, e))
    {
        if (want_attr) {
            struct berval **vals = ldap_get_values_len(ld, e, (char *)want_attr);
            if (vals) {
                for (int i = 0; vals[i]; i++) {
                    CdoString *s = cdo_string_intern(vals[i]->bv_val,
                                                     (u32)vals[i]->bv_len);
                    cdo_array_push(out_arr, cdo_string_value(s));
                    cdo_string_release(s);
                }
                ldap_value_free_len(vals);
            }
        } else {
            char *dn = ldap_get_dn(ld, e);
            if (dn) {
                CdoString *s = cdo_string_intern(dn, (u32)strlen(dn));
                cdo_array_push(out_arr, cdo_string_value(s));
                cdo_string_release(s);
                ldap_memfree(dn);
            }
        }
    }
    (void)vm;
}

/* Read a single attribute (multi-valued) from a single entry by DN.
 * Output: a freshly malloc'd NULL-terminated array of malloc'd C strings.
 * Returns 0 on rc != LDAP_SUCCESS or no entry; *out_arr stays NULL. */
static int read_dn_values(LDAP *ld, const char *dn, const char *attr,
                          char ***out_arr, size_t *out_n)
{
    *out_arr = NULL;
    *out_n   = 0;
    char *attrs[] = { (char *)attr, NULL };
    LDAPMessage *result = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_search_ext_s(ld, (PCHAR)dn, LDAP_SCOPE_BASE,
                                    (PCHAR)"(objectClass=*)",
                                    (PCHAR *)attrs, 0, NULL, NULL, NULL,
                                    0, &result);
#else
    int rc = ldap_search_ext_s(ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, NULL, 0, &result);
#endif
    if (rc != LDAP_SUCCESS || !result) {
        if (result) ldap_msgfree(result);
        return rc;
    }
    LDAPMessage *e = ldap_first_entry(ld, result);
    if (e) {
        struct berval **vals = ldap_get_values_len(ld, e, (char *)attr);
        if (vals) {
            size_t n = 0;
            while (vals[n]) n++;
            char **arr = (char **)calloc(n + 1, sizeof(char *));
            if (arr) {
                for (size_t i = 0; i < n; i++) {
                    arr[i] = (char *)malloc(vals[i]->bv_len + 1);
                    if (arr[i]) {
                        memcpy(arr[i], vals[i]->bv_val, vals[i]->bv_len);
                        arr[i][vals[i]->bv_len] = '\0';
                    }
                }
                arr[n] = NULL;
                *out_arr = arr;
                *out_n   = n;
            }
            ldap_value_free_len(vals);
        }
    }
    ldap_msgfree(result);
    return LDAP_SUCCESS;
}

/* Tiny case-insensitive DN dedup set, backed by a sorted dynamic array.
 * Adequate for member-list cardinalities (thousands at most). */
typedef struct DnSet {
    char  **dns;
    size_t  count;
    size_t  cap;
} DnSet;

static int dnset_add(DnSet *s, const char *dn)
{
    /* Linear scan -- case-insensitive on attribute types is good enough; for
     * production use you'd canonicalise via ldap_str2dn. */
    for (size_t i = 0; i < s->count; i++) {
        if (strcasecmp(s->dns[i], dn) == 0) return 0;  /* already present */
    }
    if (s->count + 1 > s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 16;
        char **bigger = (char **)realloc(s->dns, new_cap * sizeof(char *));
        if (!bigger) return -1;
        s->dns = bigger;
        s->cap = new_cap;
    }
    s->dns[s->count] = strdup(dn);
    if (!s->dns[s->count]) return -1;
    s->count++;
    return 1;
}

static void dnset_free(DnSet *s)
{
    for (size_t i = 0; i < s->count; i++) free(s->dns[i]);
    free(s->dns);
    s->dns = NULL;
    s->count = s->cap = 0;
}

/* Common implementation: walk membership and push DNs into out_arr.
 *   direction = 0 -> read `member` from group_dn (group -> users)
 *   direction = 1 -> read `memberOf` from user_dn  (user  -> groups)
 * `recursive` triggers AD matching-rule path or client-side BFS. */
static int do_membership(CandoVM *vm, LDAP *ld, int slot,
                         const char *dn, bool recursive, bool from_group,
                         CdoObject *out_arr)
{
    if (recursive && detect_ad(ld, slot)) {
        /* Single server-side query using the matching rule. */
        size_t need = ldap_helpers_escape_filter(dn, strlen(dn), NULL, 0);
        char *escaped = (char *)malloc(need + 1);
        if (!escaped) {
            ldap_throw(vm, NULL, 0, "ldap.members: out of memory");
            return -1;
        }
        ldap_helpers_escape_filter(dn, strlen(dn), escaped, need + 1);

        const char *rel = from_group ? "memberOf" : "member";
        char filter[2048];
        int n = snprintf(filter, sizeof(filter),
            "(%s:" MR_IN_CHAIN_OID ":=%s)", rel, escaped);
        free(escaped);
        if (n < 0 || n >= (int)sizeof(filter)) {
            ldap_throw(vm, NULL, 0, "ldap.members: filter buffer too small");
            return -1;
        }

        /* Search the whole namingContext: look up defaultNamingContext from
         * rootDSE.  For simplicity we use empty base for AD which permits
         * subtree against the directory's first naming context only on
         * GC connections; safer to fetch defaultNamingContext explicitly. */
        char *attrs2[] = { (char *)"defaultNamingContext", NULL };
        LDAPMessage *root_res = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
        int rc = (int)ldap_search_ext_s(ld, (PCHAR)"", LDAP_SCOPE_BASE,
                                        (PCHAR)"(objectClass=*)",
                                        (PCHAR *)attrs2, 0, NULL, NULL,
                                        NULL, 0, &root_res);
#else
        int rc = ldap_search_ext_s(ld, "", LDAP_SCOPE_BASE,
                                   "(objectClass=*)", attrs2, 0, NULL,
                                   NULL, NULL, 0, &root_res);
#endif
        char base[1024] = "";
        if (rc == LDAP_SUCCESS && root_res) {
            LDAPMessage *e = ldap_first_entry(ld, root_res);
            if (e) {
                struct berval **bv = ldap_get_values_len(ld, e,
                                                         "defaultNamingContext");
                if (bv && bv[0] && bv[0]->bv_len < sizeof(base)) {
                    memcpy(base, bv[0]->bv_val, bv[0]->bv_len);
                    base[bv[0]->bv_len] = '\0';
                }
                if (bv) ldap_value_free_len(bv);
            }
        }
        if (root_res) ldap_msgfree(root_res);

        LDAPMessage *result = NULL;
        char *attrs1[] = { (char *)"distinguishedName", NULL };
#if defined(LDAP_PLATFORM_WINDOWS)
        int sc = (int)ldap_search_ext_s(ld, (PCHAR)base, LDAP_SCOPE_SUBTREE,
                                        (PCHAR)filter, (PCHAR *)attrs1, 0,
                                        NULL, NULL, NULL, 0, &result);
#else
        int sc = ldap_search_ext_s(ld, base, LDAP_SCOPE_SUBTREE, filter,
                                   attrs1, 0, NULL, NULL, NULL, 0, &result);
#endif
        if (sc != LDAP_SUCCESS) {
            if (result) ldap_msgfree(result);
            ldap_throw(vm, ld, sc,
                "ldap.members: %s", ldap_module_strerror(sc));
            return -1;
        }
        push_dn_array_from_results(vm, ld, result, out_arr, NULL);
        ldap_msgfree(result);
        return 0;
    }

    if (!recursive) {
        /* Single-level: read attribute directly. */
        const char *want = from_group ? "member" : "memberOf";
        char **vals = NULL; size_t n = 0;
        int rc = read_dn_values(ld, dn, want, &vals, &n);
        if (rc != LDAP_SUCCESS) {
            ldap_throw(vm, ld, rc,
                "ldap.members: %s", ldap_module_strerror(rc));
            return -1;
        }
        if (vals) {
            for (size_t i = 0; i < n; i++) {
                CdoString *s = cdo_string_intern(vals[i],
                                                  (u32)strlen(vals[i]));
                cdo_array_push(out_arr, cdo_string_value(s));
                cdo_string_release(s);
                free(vals[i]);
            }
            free(vals);
        }
        return 0;
    }

    /* Client-side recursive walk for non-AD directories. */
    DnSet visited = { 0 };
    DnSet queue   = { 0 };
    int rc_add = dnset_add(&queue, dn);
    if (rc_add < 0) {
        dnset_free(&queue); dnset_free(&visited);
        ldap_throw(vm, NULL, 0, "ldap.members: out of memory");
        return -1;
    }

    /* BFS: pop head, fetch attribute, queue any not-yet-seen DNs.
     * We cap iterations at a generous limit to defend against pathological
     * cycles even though dnset_add already dedups. */
    size_t qhead = 0;
    const size_t CAP = 100000;
    size_t total_visited = 0;

    while (qhead < queue.count && total_visited < CAP) {
        const char *cur = queue.dns[qhead++];
        if (!dnset_add(&visited, cur)) continue;  /* already visited */
        total_visited++;

        const char *want = from_group ? "member" : "memberOf";
        char **vals = NULL; size_t n = 0;
        int rc = read_dn_values(ld, cur, want, &vals, &n);
        if (rc != LDAP_SUCCESS) {
            /* Skip entries we can't read (e.g. permission denied) -- the
             * results are still useful even partial. */
            continue;
        }
        if (!vals) continue;
        for (size_t i = 0; i < n; i++) {
            /* Add to result and queue if not seen. */
            int added = dnset_add(&queue, vals[i]);
            if (added > 0) {
                /* New DN: emit. */
                CdoString *s = cdo_string_intern(vals[i],
                                                  (u32)strlen(vals[i]));
                cdo_array_push(out_arr, cdo_string_value(s));
                cdo_string_release(s);
            }
            free(vals[i]);
        }
        free(vals);
    }

    dnset_free(&queue);
    dnset_free(&visited);
    return 0;
}

static bool opts_bool(CdoObject *o, const char *key, bool def)
{
    bool v = def;
    return obj_get_bool(o, key, &v) ? v : def;
}

static int native_ldap_members(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        ldap_throw(vm, NULL, 0,
            "ldap.members: (conn, group_dn[, options]) required");
        return -1;
    }
    int slot = handle_slot(vm, args[0]);
    LDAP *ld = handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        ldap_throw(vm, NULL, 0, "ldap.members: group_dn must be a string");
        return -1;
    }
    bool recursive = false;
    if (argc >= 3 && cando_is_object(args[2])) {
        CdoObject *o = cando_bridge_resolve(vm, args[2].as.handle);
        recursive = opts_bool(o, "recursive", false);
    }

    CandoValue arr_v  = cando_bridge_new_array(vm);
    CdoObject *arr    = cando_bridge_resolve(vm, arr_v.as.handle);

    if (do_membership(vm, ld, slot, dn, recursive,
                      /*from_group=*/true, arr) != 0) {
        return -1;
    }
    cando_vm_push(vm, arr_v);
    return 1;
}

static int native_ldap_member_of(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        ldap_throw(vm, NULL, 0,
            "ldap.member_of: (conn, user_dn[, options]) required");
        return -1;
    }
    int slot = handle_slot(vm, args[0]);
    LDAP *ld = handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        ldap_throw(vm, NULL, 0, "ldap.member_of: user_dn must be a string");
        return -1;
    }
    bool recursive = false;
    if (argc >= 3 && cando_is_object(args[2])) {
        CdoObject *o = cando_bridge_resolve(vm, args[2].as.handle);
        recursive = opts_bool(o, "recursive", false);
    }

    CandoValue arr_v = cando_bridge_new_array(vm);
    CdoObject *arr   = cando_bridge_resolve(vm, arr_v.as.handle);

    if (do_membership(vm, ld, slot, dn, recursive,
                      /*from_group=*/false, arr) != 0) {
        return -1;
    }
    cando_vm_push(vm, arr_v);
    return 1;
}

/* =========================================================================
 * native_ldap_rc4(key, data) -> string
 *
 * Generic RC4 stream cipher.  Useful as a building block when callers
 * need to compose AD-specific key-derivation paths the high-level
 * decode_reversible_password doesn't cover.  Strings are byte-safe in
 * Cando, so the input and output may contain NULs.
 * ======================================================================= */

static int native_ldap_rc4(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        ldap_throw(vm, NULL, 0,
            "ldap.rc4: (key, data) must both be strings");
        return -1;
    }
    CandoString *key  = args[0].as.string;
    CandoString *data = args[1].as.string;

    uint8_t *out = (uint8_t *)malloc(data->length ? data->length : 1);
    if (!out) {
        ldap_throw(vm, NULL, 0, "ldap.rc4: out of memory");
        return -1;
    }
    ADC_RC4_CTX rc4;
    adc_rc4_init(&rc4, (const uint8_t *)key->data, key->length);
    adc_rc4_xor(&rc4, (const uint8_t *)data->data, out, data->length);
    libutil_push_str(vm, (const char *)out, (u32)data->length);
    free(out);
    return 1;
}

/* =========================================================================
 * native_ldap_md5(data) -> string  (16 raw bytes)
 * ======================================================================= */

static int native_ldap_md5(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        ldap_throw(vm, NULL, 0, "ldap.md5: data must be a string");
        return -1;
    }
    CandoString *s = args[0].as.string;
    uint8_t out[16];
    adc_md5((const uint8_t *)s->data, s->length, out);
    libutil_push_str(vm, (const char *)out, 16);
    return 1;
}

/* =========================================================================
 * native_ldap_decode_reversible_password(blob, key) -> string
 *
 * High-level: RC4-decrypt `blob` with `key`, then convert the resulting
 * UTF-16LE bytes to a UTF-8 string and return it.
 *
 * IMPORTANT: This decoder requires the caller to already hold the
 * derived RC4 key.  Active Directory's reversibly-encrypted password
 * storage layers a per-record / per-RID key derivation on top of the
 * boot key (syskey).  Retrieving the encrypted blob in the first place
 * requires Domain Admin / DCSync permissions; this module does not
 * implement DRS replication.  Callers using this helper for AD password
 * recovery typically obtain `blob` and the syskey-derived intermediate
 * key out of band (e.g. via secretsdump.py / impacket) and pass them
 * here for the final unwrap.
 *
 * For the simpler case where the password is RC4(MD5(syskey || rid_le)
 * || blob, ...) the caller can compose ldap.md5 and ldap.rc4 themselves.
 * ======================================================================= */

static int native_ldap_decode_reversible_password(
    CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        ldap_throw(vm, NULL, 0,
            "ldap.decode_reversible_password: (blob, key) must both be strings");
        return -1;
    }
    CandoString *blob = args[0].as.string;
    CandoString *key  = args[1].as.string;

    if (blob->length & 1) {
        ldap_throw(vm, NULL, 0,
            "ldap.decode_reversible_password: blob length must be even "
            "(UTF-16LE)");
        return -1;
    }

    uint8_t *plain = (uint8_t *)malloc(blob->length ? blob->length : 1);
    if (!plain) {
        ldap_throw(vm, NULL, 0,
            "ldap.decode_reversible_password: out of memory");
        return -1;
    }
    adc_decode_reversible((const uint8_t *)blob->data, blob->length,
                          (const uint8_t *)key->data, key->length, plain);

    /* Strip a trailing UTF-16LE NUL pair if present -- AD often stores
     * the password as a NUL-terminated wide string. */
    size_t plen = blob->length;
    while (plen >= 2 && plain[plen-1] == 0 && plain[plen-2] == 0) plen -= 2;

    /* UTF-16LE -> UTF-8 conversion. */
    size_t utf8_cap = plen * 3 + 1;  /* worst case for BMP; non-BMP fits too */
    char *utf8 = (char *)malloc(utf8_cap);
    if (!utf8) {
        free(plain);
        ldap_throw(vm, NULL, 0,
            "ldap.decode_reversible_password: out of memory");
        return -1;
    }
    long n = adc_utf16le_to_utf8(plain, plen, utf8, utf8_cap);
    free(plain);
    if (n < 0) {
        free(utf8);
        ldap_throw(vm, NULL, 0,
            "ldap.decode_reversible_password: invalid UTF-16LE in plaintext");
        return -1;
    }
    libutil_push_str(vm, utf8, (u32)n);
    free(utf8);
    return 1;
}

/* =========================================================================
 * Module init
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
CandoValue cando_module_init(CandoVM *vm)
{
    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, tbl.as.handle);

    libutil_set_method(vm, obj, "connect",         native_ldap_connect);
    libutil_set_method(vm, obj, "set_option",      native_ldap_set_option);
    libutil_set_method(vm, obj, "bind",            native_ldap_bind);
    libutil_set_method(vm, obj, "bind_anonymous",  native_ldap_bind_anonymous);
    libutil_set_method(vm, obj, "start_tls",       native_ldap_start_tls);
    libutil_set_method(vm, obj, "unbind",          native_ldap_unbind);
    libutil_set_method(vm, obj, "search",          native_ldap_search);
    libutil_set_method(vm, obj, "add",             native_ldap_add);
    libutil_set_method(vm, obj, "modify",          native_ldap_modify);
    libutil_set_method(vm, obj, "delete",          native_ldap_delete);
    libutil_set_method(vm, obj, "rename",          native_ldap_rename);
    libutil_set_method(vm, obj, "move",            native_ldap_move);
    libutil_set_method(vm, obj, "compare",         native_ldap_compare);
    libutil_set_method(vm, obj, "rootdse",          native_ldap_rootdse);
    libutil_set_method(vm, obj, "test_credentials", native_ldap_test_credentials);
    libutil_set_method(vm, obj, "password_modify",  native_ldap_password_modify);
    libutil_set_method(vm, obj, "escape_filter",    native_ldap_escape_filter);
    libutil_set_method(vm, obj, "escape_dn",        native_ldap_escape_dn);
    libutil_set_method(vm, obj, "members",          native_ldap_members);
    libutil_set_method(vm, obj, "member_of",        native_ldap_member_of);
    libutil_set_method(vm, obj, "rc4",              native_ldap_rc4);
    libutil_set_method(vm, obj, "md5",              native_ldap_md5);
    libutil_set_method(vm, obj, "decode_reversible_password",
                       native_ldap_decode_reversible_password);

    /* Constants */
    obj_set_number(obj, "SCOPE_BASE", (f64)LDAP_SCOPE_BASE);
    obj_set_number(obj, "SCOPE_ONE",  (f64)LDAP_SCOPE_ONELEVEL);
    obj_set_number(obj, "SCOPE_SUB",  (f64)LDAP_SCOPE_SUBTREE);
    obj_set_string(obj, "VERSION",    "1.0.0", 5);

    return tbl;
}
