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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* =========================================================================
 * Module sentinel
 *
 * Connection objects exposed to the script carry a magic key whose value
 * is the LDAP* pointer cast to f64.  This lets us verify a value really
 * is a connection -- not an arbitrary user object -- before we cast back.
 * ======================================================================= */

#define LDAP_HANDLE_KEY    "__ldap_handle"
#define LDAP_CLOSED_KEY    "__ldap_closed"
#define LDAP_LAST_ERR_KEY  "__ldap_last_error"
#define LDAP_LAST_CODE_KEY "__ldap_last_code"

/* Module-global last error.  set by ldap_module_set_error and read by the
 * native_ldap_last_error native.  Plain global is fine -- the host VM is
 * single-threaded for native calls. */
static char  g_last_error[512] = "";
static int   g_last_code       = 0;

static void ldap_module_set_error(int code, const char *msg)
{
    g_last_code = code;
    if (msg) {
        size_t n = strlen(msg);
        if (n >= sizeof(g_last_error)) n = sizeof(g_last_error) - 1;
        memcpy(g_last_error, msg, n);
        g_last_error[n] = '\0';
    } else {
        g_last_error[0] = '\0';
    }
}

static void ldap_module_clear_error(void)
{
    g_last_code     = 0;
    g_last_error[0] = '\0';
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

static void obj_set_bool(CdoObject *obj, const char *key, bool value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_bool(value), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Connection handle: LDAP* boxed inside an object
 * ======================================================================= */

/* Box an LDAP* inside a fresh object.  Returns the object value (already
 * pinned in the bridge handle table). */
static CandoValue make_handle(CandoVM *vm, void *ld)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);

    /* Stuff the LDAP* into a number field via a uintptr_t round-trip.  f64
     * has 53 bits of mantissa; on 64-bit systems pointers fit -- but to be
     * safe on all targets we split into two 32-bit halves. */
    uintptr_t p = (uintptr_t)ld;
    /* Lower 32 bits and upper 32 bits stored separately so a 64-bit
     * pointer can be reconstructed losslessly. */
    f64 lo = (f64)(u32)(p & 0xFFFFFFFFu);
    f64 hi = (f64)(u32)((p >> 16) >> 16);  /* avoid shift warnings on 32-bit */

    CdoString *kh = cdo_string_intern(LDAP_HANDLE_KEY,
                                       (u32)strlen(LDAP_HANDLE_KEY));
    cdo_object_rawset(obj, kh, cdo_number(lo), FIELD_NONE);
    cdo_string_release(kh);

    obj_set_number(obj, LDAP_HANDLE_KEY "_hi", hi);
    obj_set_bool  (obj, LDAP_CLOSED_KEY,  false);

    return v;
}

/* Recover an LDAP* from a connection object, or NULL if v is not a valid
 * (open) connection.  On error sets a vm error message. */
static void *handle_unwrap(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) {
        cando_vm_error(vm, "ldap: expected connection object");
        return NULL;
    }
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);

    bool closed = false;
    obj_get_bool(obj, LDAP_CLOSED_KEY, &closed);
    if (closed) {
        cando_vm_error(vm, "ldap: connection has been unbound");
        return NULL;
    }

    f64 lo_d = 0, hi_d = 0;
    if (!obj_get_number(obj, LDAP_HANDLE_KEY,         &lo_d) ||
        !obj_get_number(obj, LDAP_HANDLE_KEY "_hi",   &hi_d)) {
        cando_vm_error(vm, "ldap: value is not a connection object");
        return NULL;
    }
    uintptr_t p = (uintptr_t)((u32)lo_d) |
                  (((uintptr_t)((u32)hi_d) << 16) << 16);
    return (void *)p;
}

static void handle_mark_closed(CandoVM *vm, CandoValue v)
{
    if (!cando_is_object(v)) return;
    CdoObject *obj = cando_bridge_resolve(vm, v.as.handle);
    obj_set_bool(obj, LDAP_CLOSED_KEY, true);
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
        cando_vm_error(vm, "ldap.connect: URI must be a string");
        return -1;
    }

    LDAP *ld = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    /* winldap accepts "ldap://host:port" via ldap_initA but the cleanest
     * cross-platform path is to parse out host & port and use ldap_init.
     * For simplicity we accept the URI directly: many builds of wldap32
     * support ldap_sslinit / ldap_init with a hostname.  Strip the scheme
     * and pass host + port. */
    const char *host = uri;
    int port = LDAP_PORT;
    int use_ssl = 0;
    if (strncmp(uri, "ldaps://", 8) == 0) { host = uri + 8; use_ssl = 1; port = LDAP_SSL_PORT; }
    else if (strncmp(uri, "ldap://", 7) == 0) { host = uri + 7; }

    /* Mutable copy so we can split host:port */
    char hostbuf[512];
    size_t n = strlen(host);
    if (n >= sizeof(hostbuf)) {
        cando_vm_error(vm, "ldap.connect: URI too long");
        return -1;
    }
    memcpy(hostbuf, host, n + 1);
    char *colon = strrchr(hostbuf, ':');
    /* Only treat as port if it's after a host segment with no slashes */
    if (colon && !strchr(colon, '/')) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) port = use_ssl ? LDAP_SSL_PORT : LDAP_PORT;
    }
    ld = use_ssl ? ldap_sslinit(hostbuf, port, 1)
                 : ldap_init   (hostbuf, port);
    if (!ld) {
        ldap_module_set_error((int)LdapGetLastError(),
                              "ldap.connect: ldap_init failed");
        cando_vm_error(vm, "ldap.connect: failed to initialise (%lu)",
                       (unsigned long)LdapGetLastError());
        return -1;
    }
#else
    int rc = ldap_initialize(&ld, uri);
    if (rc != LDAP_SUCCESS || !ld) {
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.connect: %s", ldap_module_strerror(rc));
        return -1;
    }
    /* Default to LDAPv3. */
    int version = LDAP_VERSION3;
    ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
#endif

    ldap_module_clear_error();
    cando_vm_push(vm, make_handle(vm, ld));
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
        cando_vm_error(vm, "ldap.set_option: (conn, name, value) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *name = libutil_arg_cstr_at(args, argc, 1);
    if (!name) {
        cando_vm_error(vm, "ldap.set_option: option name must be a string");
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
    } else {
        cando_vm_error(vm, "ldap.set_option: unknown option '%s'", name);
        return -1;
    }

    if (rc != LDAP_SUCCESS) {
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.set_option: %s", ldap_module_strerror(rc));
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
        cando_vm_error(vm, "ldap.bind: connection required");
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
        ldap_module_set_error(irc, ldap_module_strerror(irc));
        cando_vm_error(vm, "ldap.bind: %s", ldap_module_strerror(irc));
        return -1;
    }
    ldap_module_clear_error();
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_bind_anonymous(conn) -> bool
 * ======================================================================= */

static int native_ldap_bind_anonymous(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ldap.bind_anonymous: connection required");
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
        ldap_module_set_error(irc, ldap_module_strerror(irc));
        cando_vm_error(vm, "ldap.bind_anonymous: %s", ldap_module_strerror(irc));
        return -1;
    }
    ldap_module_clear_error();
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_start_tls(conn) -> bool
 * ======================================================================= */

static int native_ldap_start_tls(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ldap.start_tls: connection required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
#if defined(LDAP_PLATFORM_WINDOWS)
    /* winldap exposes ldap_start_tls_s(ld, serverctrls, clientctrls) */
    int irc = (int)ldap_start_tls_s(ld, NULL, NULL);
#else
    int irc = ldap_start_tls_s(ld, NULL, NULL);
#endif
    if (irc != LDAP_SUCCESS) {
        ldap_module_set_error(irc, ldap_module_strerror(irc));
        cando_vm_error(vm, "ldap.start_tls: %s", ldap_module_strerror(irc));
        return -1;
    }
    ldap_module_clear_error();
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_unbind(conn) -> bool
 * ======================================================================= */

static int native_ldap_unbind(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_object(args[0])) {
        cando_vm_error(vm, "ldap.unbind: connection required");
        return -1;
    }
    /* Allow unbind on already-closed connection -- no-op. */
    CdoObject *obj = cando_bridge_resolve(vm, args[0].as.handle);
    bool closed = false;
    obj_get_bool(obj, LDAP_CLOSED_KEY, &closed);
    if (closed) {
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }

    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;

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
        cando_vm_error(vm, "ldap.search: (conn, options) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    if (!cando_is_object(args[1])) {
        cando_vm_error(vm, "ldap.search: options must be an object");
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
        cando_vm_error(vm, "ldap.search: options.base is required");
        return -1;
    }
    obj_get_string(opts, "filter", &filter, &filt_len);
    if (obj_get_string(opts, "scope", &scope_s, &scope_len)) {
        if (!ldap_module_parse_scope(scope_s, &scope)) {
            cando_vm_error(vm, "ldap.search: unknown scope '%s'", scope_s);
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
                cando_vm_error(vm, "ldap.search: out of memory");
                return -1;
            }
            for (u32 i = 0; i < n; i++) {
                CdoValue elem;
                if (!cdo_array_rawget_idx(arr, i, &elem) ||
                    elem.tag != CDO_STRING) {
                    free_str_array(attrs);
                    cando_vm_error(vm,
                        "ldap.search: attrs[%u] must be a string",
                        (unsigned)i);
                    return -1;
                }
                size_t l = elem.as.string->length;
                char *dup = (char *)malloc(l + 1);
                if (!dup) {
                    free_str_array(attrs);
                    cando_vm_error(vm, "ldap.search: out of memory");
                    return -1;
                }
                memcpy(dup, elem.as.string->data, l);
                dup[l] = '\0';
                attrs[i] = dup;
            }
            attrs[n] = NULL;
        }
    }

    LDAPMessage *result = NULL;
#if defined(LDAP_PLATFORM_WINDOWS)
    struct l_timeval tv;  tv.tv_sec = time_limit; tv.tv_usec = 0;
    struct l_timeval *tvp = (time_limit > 0) ? &tv : NULL;
    int rc = (int)ldap_search_ext_s(ld, (PCHAR)base, scope, (PCHAR)filter,
                                    (PCHAR *)attrs, 0, NULL, NULL, tvp,
                                    size_limit, &result);
#else
    struct timeval tv;  tv.tv_sec = time_limit; tv.tv_usec = 0;
    struct timeval *tvp = (time_limit > 0) ? &tv : NULL;
    int rc = ldap_search_ext_s(ld, base, scope, filter, attrs, 0,
                               NULL, NULL, tvp, size_limit, &result);
#endif

    if (rc != LDAP_SUCCESS) {
        if (result) ldap_msgfree(result);
        free_str_array(attrs);
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.search: %s", ldap_module_strerror(rc));
        return -1;
    }

    /* Build [{dn, attributes:{name:[vals]}}, ...] */
    CandoValue out_arr_v = cando_bridge_new_array(vm);
    CdoObject *out_arr   = cando_bridge_resolve(vm, out_arr_v.as.handle);

    for (LDAPMessage *e = ldap_first_entry(ld, result); e != NULL;
         e = ldap_next_entry(ld, e))
    {
        CandoValue ent_v = cando_bridge_new_object(vm);
        CdoObject *ent   = cando_bridge_resolve(vm, ent_v.as.handle);

        /* dn */
        char *dn = ldap_get_dn(ld, e);
        if (dn) {
            obj_set_string(ent, "dn", dn, (u32)strlen(dn));
#if defined(LDAP_PLATFORM_WINDOWS)
            ldap_memfree(dn);
#else
            ldap_memfree(dn);
#endif
        }

        /* attributes object */
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

        CdoString *kattr = cdo_string_intern("attributes", 10);
        cdo_object_rawset(ent, kattr, cdo_object_value(attro), FIELD_NONE);
        cdo_string_release(kattr);

        cdo_array_push(out_arr, cdo_object_value(ent));
    }

    ldap_msgfree(result);
    free_str_array(attrs);

    ldap_module_clear_error();
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
        if (!arr) { cando_vm_error(vm, "%s: out of memory", what); return NULL; }
        size_t l = v.as.string->length;
        arr[0] = (char *)malloc(l + 1);
        if (!arr[0]) { free(arr); cando_vm_error(vm, "%s: out of memory", what); return NULL; }
        memcpy(arr[0], v.as.string->data, l);
        arr[0][l] = '\0';
        arr[1] = NULL;
        return arr;
    }
    if (v.tag == CDO_ARRAY) {
        CdoObject *a = v.as.object;
        u32 n = cdo_array_len(a);
        char **arr = (char **)calloc((size_t)n + 1, sizeof(char *));
        if (!arr) { cando_vm_error(vm, "%s: out of memory", what); return NULL; }
        for (u32 i = 0; i < n; i++) {
            CdoValue ev;
            if (!cdo_array_rawget_idx(a, i, &ev) || ev.tag != CDO_STRING) {
                for (u32 j = 0; j < i; j++) free(arr[j]);
                free(arr);
                cando_vm_error(vm, "%s: value at index %u must be a string",
                               what, (unsigned)i);
                return NULL;
            }
            size_t l = ev.as.string->length;
            arr[i] = (char *)malloc(l + 1);
            if (!arr[i]) {
                for (u32 j = 0; j < i; j++) free(arr[j]);
                free(arr);
                cando_vm_error(vm, "%s: out of memory", what);
                return NULL;
            }
            memcpy(arr[i], ev.as.string->data, l);
            arr[i][l] = '\0';
        }
        arr[n] = NULL;
        return arr;
    }
    cando_vm_error(vm, "%s: value must be a string or array of strings", what);
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
        cando_vm_error(s->vm, "ldap.add: out of memory");
        s->failed = true;
        return false;
    }
    m->mod_op     = LDAP_MOD_ADD;
    m->mod_type   = (char *)malloc((size_t)key->length + 1);
    if (!m->mod_type) {
        free(m);
        for (int j = 0; vals[j]; j++) free(vals[j]);
        free(vals);
        cando_vm_error(s->vm, "ldap.add: out of memory");
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
            cando_vm_error(s->vm, "ldap.add: out of memory");
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
        cando_vm_error(vm, "ldap.add: out of memory");
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
    if (!mods) { cando_vm_error(vm, "ldap.modify: out of memory"); return 0; }

    for (u32 i = 0; i < n; i++) {
        CdoValue ev;
        if (!cdo_array_rawget_idx(arr, i, &ev) || ev.tag != CDO_OBJECT) {
            cando_vm_error(vm, "ldap.modify: mods[%u] must be an object",
                           (unsigned)i);
            goto fail;
        }
        CdoObject *mobj = ev.as.object;

        const char *op = NULL, *attr = NULL;
        size_t op_len = 0, attr_len = 0;
        if (!obj_get_string(mobj, "op", &op, &op_len) ||
            !obj_get_string(mobj, "attr", &attr, &attr_len)) {
            cando_vm_error(vm,
                "ldap.modify: mods[%u] requires .op and .attr",
                (unsigned)i);
            goto fail;
        }

        int op_code;
        if (!ldap_helpers_parse_mod_op(op, &op_code)) {
            cando_vm_error(vm, "ldap.modify: unknown op '%s'", op);
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
            cando_vm_error(vm, "ldap.modify: out of memory");
            goto fail;
        }
        m->mod_op     = op_code;
        m->mod_type   = (char *)malloc(attr_len + 1);
        if (!m->mod_type) {
            free(m);
            if (vals) { for (int j = 0; vals[j]; j++) free(vals[j]); free(vals); }
            cando_vm_error(vm, "ldap.modify: out of memory");
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
        cando_vm_error(vm, "ldap.add: (conn, dn, attrs) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        cando_vm_error(vm, "ldap.add: dn must be a string");
        return -1;
    }
    if (!cando_is_object(args[2])) {
        cando_vm_error(vm, "ldap.add: attrs must be an object");
        return -1;
    }
    CdoObject *attrs = cando_bridge_resolve(vm, args[2].as.handle);
    if (attrs->kind == OBJ_ARRAY) {
        cando_vm_error(vm, "ldap.add: attrs must be an object, not an array");
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
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.add: %s", ldap_module_strerror(rc));
        return -1;
    }
    ldap_module_clear_error();
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
        cando_vm_error(vm, "ldap.modify: (conn, dn, mods) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        cando_vm_error(vm, "ldap.modify: dn must be a string");
        return -1;
    }
    if (!cando_is_object(args[2])) {
        cando_vm_error(vm, "ldap.modify: mods must be an array");
        return -1;
    }
    CdoObject *modarr = cando_bridge_resolve(vm, args[2].as.handle);
    if (modarr->kind != OBJ_ARRAY) {
        cando_vm_error(vm, "ldap.modify: mods must be an array of objects");
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
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.modify: %s", ldap_module_strerror(rc));
        return -1;
    }
    ldap_module_clear_error();
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* =========================================================================
 * native_ldap_delete(conn, dn) -> bool
 * ======================================================================= */

static int native_ldap_delete(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        cando_vm_error(vm, "ldap.delete: (conn, dn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn = libutil_arg_cstr_at(args, argc, 1);
    if (!dn) {
        cando_vm_error(vm, "ldap.delete: dn must be a string");
        return -1;
    }
#if defined(LDAP_PLATFORM_WINDOWS)
    int rc = (int)ldap_delete_ext_s(ld, (PCHAR)dn, NULL, NULL);
#else
    int rc = ldap_delete_ext_s(ld, dn, NULL, NULL);
#endif
    if (rc != LDAP_SUCCESS) {
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.delete: %s", ldap_module_strerror(rc));
        return -1;
    }
    ldap_module_clear_error();
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
        cando_vm_error(vm, "ldap.rename: (conn, dn, new_rdn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn      = libutil_arg_cstr_at(args, argc, 1);
    const char *new_rdn = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !new_rdn) {
        cando_vm_error(vm, "ldap.rename: dn and new_rdn must be strings");
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
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.rename: %s", ldap_module_strerror(rc));
        return -1;
    }
    ldap_module_clear_error();
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
        cando_vm_error(vm, "ldap.move: (conn, dn, new_parent_dn) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn         = libutil_arg_cstr_at(args, argc, 1);
    const char *new_parent = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !new_parent) {
        cando_vm_error(vm,
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
            cando_vm_error(vm, "ldap.move: out of memory");
            return -1;
        }
        if (!ldap_helpers_extract_rdn(dn, rdn_buf, need)) {
            free(rdn_buf);
            cando_vm_error(vm, "ldap.move: invalid dn");
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
        ldap_module_set_error(rc, ldap_module_strerror(rc));
        cando_vm_error(vm, "ldap.move: %s", ldap_module_strerror(rc));
        return -1;
    }
    ldap_module_clear_error();
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
        cando_vm_error(vm, "ldap.compare: (conn, dn, attr, value) required");
        return -1;
    }
    LDAP *ld = (LDAP *)handle_unwrap(vm, args[0]);
    if (!ld) return -1;
    const char *dn   = libutil_arg_cstr_at(args, argc, 1);
    const char *attr = libutil_arg_cstr_at(args, argc, 2);
    if (!dn || !attr || !cando_is_string(args[3])) {
        cando_vm_error(vm, "ldap.compare: dn, attr, value must be strings");
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
        ldap_module_clear_error();
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    if (rc == LDAP_COMPARE_FALSE) {
        ldap_module_clear_error();
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    ldap_module_set_error(rc, ldap_module_strerror(rc));
    cando_vm_error(vm, "ldap.compare: %s", ldap_module_strerror(rc));
    return -1;
}

/* =========================================================================
 * native_ldap_last_error() -> { code, message } | null
 * ======================================================================= */

static int native_ldap_last_error(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    if (g_last_code == 0 && g_last_error[0] == '\0') {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, v.as.handle);
    obj_set_number(o, "code", (f64)g_last_code);
    obj_set_string(o, "message", g_last_error, (u32)strlen(g_last_error));
    cando_vm_push(vm, v);
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
    libutil_set_method(vm, obj, "last_error",      native_ldap_last_error);

    /* Constants */
    obj_set_number(obj, "SCOPE_BASE", (f64)LDAP_SCOPE_BASE);
    obj_set_number(obj, "SCOPE_ONE",  (f64)LDAP_SCOPE_ONELEVEL);
    obj_set_number(obj, "SCOPE_SUB",  (f64)LDAP_SCOPE_SUBTREE);
    obj_set_string(obj, "VERSION",    "1.0.0", 5);

    return tbl;
}
