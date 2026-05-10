/*
 * lib/https.c -- HTTPS standard library for Cando.
 *
 * Thin TLS wrapper over the shared HTTP machinery in http.c / httputil.c.
 *
 *   https.request(opts)             -- always forces TLS
 *   https.get(url)                  -- TLS convenience GET
 *   https.createServer(opts, cb)    -- opts.cert / opts.key are PEM strings
 *
 * Must compile with gcc -std=c11.
 */

#include "https.h"
#include "http.h"
#include "httputil.h"
#include "sockutil.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/string.h"

#include <openssl/ssl.h>

#include <string.h>

/* =========================================================================
 * Client: reuse shared http_do_request_native with is_tls_hint = true.
 * ===================================================================== */

static int https_request_fn(CandoVM *vm, int argc, CandoValue *args)
{
    return http_do_request_native(vm, argc, args, true);
}

static int https_get_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (!libutil_require_str_at(vm, args, argc, 0, "https.get")) return -1;
    /* Build a minimal options object with method=GET and let the workhorse
     * handle TLS and redirect (scheme will be https regardless). */
    CandoValue opts_val = cando_bridge_new_object(vm);
    CdoObject *opts     = cando_bridge_resolve(vm, cando_as_handle(opts_val));

    CdoString *kurl = cdo_string_intern("url", 3);
    cdo_object_rawset(opts, kurl,
                      cdo_string_value(cdo_string_new(cando_as_string(args[0])->data,
                                                      cando_as_string(args[0])->length)),
                      FIELD_NONE);
    cdo_string_release(kurl);

    CdoString *kmeth = cdo_string_intern("method", 6);
    cdo_object_rawset(opts, kmeth,
                      cdo_string_value(cdo_string_new("GET", 3)),
                      FIELD_NONE);
    cdo_string_release(kmeth);

    CandoValue single[1] = { opts_val };
    return http_do_request_native(vm, 1, single, true);
}

/* =========================================================================
 * Server: https.createServer({cert, key}, callback)
 * ===================================================================== */

/* Extract a PEM string field from an options object.  Returns NULL if missing. */
static CdoString *opts_get_string_field(CdoObject *obj, const char *name)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    CdoValue   v   = cdo_null();
    bool ok = cdo_object_rawget(obj, key, &v);
    cdo_string_release(key);
    if (!ok || v.tag != CDO_STRING) { cdo_value_release(v); return NULL; }
    CdoString *s = cdo_string_retain(v.as.string);
    cdo_value_release(v);
    return s;
}

static int https_create_server_fn(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        cando_vm_error(vm, "https.createServer: expected (opts, callback)");
        return -1;
    }
    if (!cando_is_object(args[0])) {
        cando_vm_error(vm, "https.createServer: opts must be an object with cert/key");
        return -1;
    }
    CdoObject *opts = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    CdoString *cert = opts_get_string_field(opts, "cert");
    CdoString *key  = opts_get_string_field(opts, "key");
    if (!cert || !key) {
        if (cert) cdo_string_release(cert);
        if (key)  cdo_string_release(key);
        cando_vm_error(vm, "https.createServer: opts.cert and opts.key are required (PEM strings)");
        return -1;
    }

    char errmsg[128] = {0};
    SSL_CTX *ctx = sockutil_build_server_ssl_ctx(cert->data, cert->length,
                                                 key->data,  key->length,
                                                 NULL,
                                                 errmsg, sizeof(errmsg));
    cdo_string_release(cert);
    cdo_string_release(key);
    if (!ctx) {
        cando_vm_error(vm, "https.createServer: %s", errmsg);
        return -1;
    }

    /* Shared server implementation takes ownership of ctx. */
    return http_create_server_native_impl(vm, args[1], (struct SSL_CTX_st *)ctx);
}

/* =========================================================================
 * Registration
 * ===================================================================== */

void cando_lib_https_register(CandoVM *vm)
{
    http_one_time_init();

    CandoValue https_val = cando_bridge_new_object(vm);
    CdoObject *https_obj = cando_bridge_resolve(vm, cando_as_handle(https_val));

    libutil_set_method(vm, https_obj, "request",      https_request_fn);
    libutil_set_method(vm, https_obj, "get",          https_get_fn);
    libutil_set_method(vm, https_obj, "createServer", https_create_server_fn);

    cando_vm_set_global(vm, "https", https_val, true);

    /* Share the same `_meta` tables as http: TLS only changes the transport,
     * not the request/response shape, so users register methods once. */
    http_register_meta_tables(vm);
}
