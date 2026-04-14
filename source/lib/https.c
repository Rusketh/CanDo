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
#include "libutil.h"
#include "../vm/bridge.h"
#include "../vm/vm.h"
#include "../object/object.h"
#include "../object/string.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/x509.h>

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
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "https.get: expected url string");
        return -1;
    }
    /* Build a minimal options object with method=GET and let the workhorse
     * handle TLS and redirect (scheme will be https regardless). */
    CandoValue opts_val = cando_bridge_new_object(vm);
    CdoObject *opts     = cando_bridge_resolve(vm, opts_val.as.handle);

    CdoString *kurl = cdo_string_intern("url", 3);
    cdo_object_rawset(opts, kurl,
                      cdo_string_value(cdo_string_new(args[0].as.string->data,
                                                      args[0].as.string->length)),
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

static SSL_CTX *build_server_ctx(const char *cert_pem, u32 cert_len,
                                 const char *key_pem,  u32 key_len,
                                 char *err, usize errlen)
{
    http_one_time_init();

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { snprintf(err, errlen, "SSL_CTX_new failed"); return NULL; }

    /* Load certificate (supports chains: repeatedly read X509 from BIO). */
    BIO *cbio = BIO_new_mem_buf(cert_pem, (int)cert_len);
    if (!cbio) { SSL_CTX_free(ctx); snprintf(err, errlen, "cert BIO failed"); return NULL; }

    X509 *first = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    if (!first) {
        BIO_free(cbio);
        SSL_CTX_free(ctx);
        snprintf(err, errlen, "invalid PEM certificate");
        return NULL;
    }
    if (SSL_CTX_use_certificate(ctx, first) != 1) {
        X509_free(first);
        BIO_free(cbio);
        SSL_CTX_free(ctx);
        snprintf(err, errlen, "SSL_CTX_use_certificate failed");
        return NULL;
    }
    X509_free(first);
    /* Add any extra chain certs. */
    X509 *extra;
    while ((extra = PEM_read_bio_X509(cbio, NULL, NULL, NULL)) != NULL) {
        if (SSL_CTX_add_extra_chain_cert(ctx, extra) != 1) {
            X509_free(extra);
            /* non-fatal */
            break;
        }
        /* ownership transferred to ctx on success */
    }
    BIO_free(cbio);

    /* Load private key. */
    BIO *kbio = BIO_new_mem_buf(key_pem, (int)key_len);
    if (!kbio) { SSL_CTX_free(ctx); snprintf(err, errlen, "key BIO failed"); return NULL; }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(kbio, NULL, NULL, NULL);
    BIO_free(kbio);
    if (!pkey) { SSL_CTX_free(ctx); snprintf(err, errlen, "invalid PEM private key"); return NULL; }

    if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ctx);
        snprintf(err, errlen, "SSL_CTX_use_PrivateKey failed");
        return NULL;
    }
    EVP_PKEY_free(pkey);

    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        snprintf(err, errlen, "cert/key mismatch");
        return NULL;
    }
    return ctx;
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
    CdoObject *opts = cando_bridge_resolve(vm, args[0].as.handle);
    CdoString *cert = opts_get_string_field(opts, "cert");
    CdoString *key  = opts_get_string_field(opts, "key");
    if (!cert || !key) {
        if (cert) cdo_string_release(cert);
        if (key)  cdo_string_release(key);
        cando_vm_error(vm, "https.createServer: opts.cert and opts.key are required (PEM strings)");
        return -1;
    }

    char errmsg[128];
    SSL_CTX *ctx = build_server_ctx(cert->data, cert->length,
                                    key->data,  key->length,
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
    CdoObject *https_obj = cando_bridge_resolve(vm, https_val.as.handle);

    libutil_set_method(vm, https_obj, "request",      https_request_fn);
    libutil_set_method(vm, https_obj, "get",          https_get_fn);
    libutil_set_method(vm, https_obj, "createServer", https_create_server_fn);

    cando_vm_set_global(vm, "https", https_val, true);
}
