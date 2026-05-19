/*
 * lib/crypto.c -- Cryptography standard library for Cando.
 *
 * Backed by OpenSSL 3.x (-lcrypto), which the rest of the runtime
 * already links for HTTPS / secure sockets.
 *
 * Calling style is one-shot throughout: no stateful builders, just
 * `crypto.sha256(data, "hex")`, `crypto.encrypt(algo, key, iv, ...)`,
 * etc.  Every routine that produces bytes accepts an optional trailing
 * `encoding` argument selecting the script-visible output format:
 *
 *   "hex"        (default) lowercase hex
 *   "base64"     RFC 4648 with `+/` and `=` padding
 *   "base64url"  RFC 4648 §5 url-safe, no padding
 *   "bytes"      raw binary string (CanDo strings are byte arrays)
 *
 * Inputs accept either a CanDo string (treated as raw bytes; length is
 * explicit so embedded NULs are fine) or, where the function takes a
 * key/iv/data, an explicit-byte string.  Keys and PEM blobs are also
 * strings.  No opaque handle types are introduced -- the surface stays
 * data-in / data-out so scripts compose cleanly.
 *
 * Must compile with gcc -std=c11.
 */

#include "crypto.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"
#include "../object/array.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/param_build.h>

/* =========================================================================
 * Output-encoding helper
 *
 * Every crypto routine that returns bytes funnels through this.  The
 * encoding is read from argv[enc_idx] (a string) and defaults to
 * "hex" when the argument is absent or non-string.
 * ======================================================================= */

typedef enum CryptoEnc {
    CRYPTO_ENC_HEX = 0,
    CRYPTO_ENC_BASE64,
    CRYPTO_ENC_BASE64URL,
    CRYPTO_ENC_BYTES,
} CryptoEnc;

static CryptoEnc parse_encoding(const char *s)
{
    if (!s)                              return CRYPTO_ENC_HEX;
    if (strcmp(s, "hex")       == 0)     return CRYPTO_ENC_HEX;
    if (strcmp(s, "base64")    == 0)     return CRYPTO_ENC_BASE64;
    if (strcmp(s, "base64url") == 0)     return CRYPTO_ENC_BASE64URL;
    if (strcmp(s, "bytes")     == 0)     return CRYPTO_ENC_BYTES;
    if (strcmp(s, "binary")    == 0)     return CRYPTO_ENC_BYTES; /* alias */
    if (strcmp(s, "raw")       == 0)     return CRYPTO_ENC_BYTES; /* alias */
    return CRYPTO_ENC_HEX;
}

static CryptoEnc enc_from_args(CandoValue *args, int argc, int idx)
{
    if (idx >= argc || !cando_is_string(args[idx])) return CRYPTO_ENC_HEX;
    return parse_encoding(cando_as_string(args[idx])->data);
}

/* enc_from_args_or -- like enc_from_args but with an explicit default
 * kind.  Use BYTES as the default for routines whose output is key
 * material / ciphertext (cipher, KDF, DH) rather than a digest for
 * display (hash, HMAC). */
static CryptoEnc enc_from_args_or(CandoValue *args, int argc, int idx,
                                  CryptoEnc dflt)
{
    if (idx >= argc || !cando_is_string(args[idx])) return dflt;
    return parse_encoding(cando_as_string(args[idx])->data);
}

/* Hex encode a byte buffer to lowercase hex.  Caller owns `out` (must
 * be at least 2*len + 1 bytes). */
static void hex_encode_into(const unsigned char *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[ in[i]       & 0xF];
    }
    out[len * 2] = '\0';
}

/* Hex decode.  Returns the number of bytes written, or -1 on bad
 * input.  out must be at least len/2 bytes. */
static long hex_decode(const char *in, size_t len, unsigned char *out)
{
    if (len % 2 != 0) return -1;
    for (size_t i = 0; i < len; i += 2) {
        int hi, lo;
        char a = in[i], b = in[i + 1];
        if      (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else return -1;
        if      (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else return -1;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return (long)(len / 2);
}

/* Base64 alphabets. */
static const char b64_std_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64_decode_value(char c, bool urlsafe)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (urlsafe) {
        if (c == '-') return 62;
        if (c == '_') return 63;
    } else {
        if (c == '+') return 62;
        if (c == '/') return 63;
    }
    return -1;
}

/* Encode `in` to base64 (or base64url with no padding when url=true).
 * Returns malloc'd buffer; caller frees with cando_free.  out_len is
 * the number of bytes written (excluding the NUL terminator). */
static char *b64_encode_dup(const unsigned char *in, size_t len,
                            bool urlsafe, size_t *out_len)
{
    const char *abc = urlsafe ? b64_url_alphabet : b64_std_alphabet;
    /* For url-safe we drop padding; size = ceil(len*4/3) without '='s. */
    size_t full_triples = len / 3;
    size_t tail         = len - full_triples * 3;
    size_t encoded_len  = full_triples * 4 + (tail ? (urlsafe ? (tail == 1 ? 2 : 3) : 4) : 0);
    char *out = (char *)cando_alloc(encoded_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < full_triples * 3; i += 3) {
        uint32_t triple = ((uint32_t)in[i]     << 16)
                        | ((uint32_t)in[i + 1] <<  8)
                        |  (uint32_t)in[i + 2];
        out[j++] = abc[(triple >> 18) & 0x3F];
        out[j++] = abc[(triple >> 12) & 0x3F];
        out[j++] = abc[(triple >>  6) & 0x3F];
        out[j++] = abc[ triple        & 0x3F];
    }
    if (tail == 1) {
        uint32_t v = (uint32_t)in[full_triples * 3] << 16;
        out[j++] = abc[(v >> 18) & 0x3F];
        out[j++] = abc[(v >> 12) & 0x3F];
        if (!urlsafe) { out[j++] = '='; out[j++] = '='; }
    } else if (tail == 2) {
        uint32_t v = ((uint32_t)in[full_triples * 3]     << 16)
                   | ((uint32_t)in[full_triples * 3 + 1] <<  8);
        out[j++] = abc[(v >> 18) & 0x3F];
        out[j++] = abc[(v >> 12) & 0x3F];
        out[j++] = abc[(v >>  6) & 0x3F];
        if (!urlsafe) { out[j++] = '='; }
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* Decode either base64 or base64url input.  Padding `=` is optional
 * (accepted but not required).  Returns malloc'd buffer + len, or
 * NULL on malformed input. */
static unsigned char *b64_decode_dup(const char *in, size_t in_len,
                                     bool urlsafe, size_t *out_len)
{
    /* Strip trailing '=' padding. */
    while (in_len > 0 && in[in_len - 1] == '=') in_len--;

    size_t full = in_len / 4;
    size_t rem  = in_len % 4;
    if (rem == 1) return NULL;     /* invalid base64 group of 1 char */

    size_t decoded_len = full * 3 + (rem == 2 ? 1 : rem == 3 ? 2 : 0);
    unsigned char *out = (unsigned char *)cando_alloc(decoded_len + 1);
    size_t j = 0;

    for (size_t i = 0; i < full * 4; i += 4) {
        int a = b64_decode_value(in[i],     urlsafe);
        int b = b64_decode_value(in[i + 1], urlsafe);
        int c = b64_decode_value(in[i + 2], urlsafe);
        int d = b64_decode_value(in[i + 3], urlsafe);
        if (a < 0 || b < 0 || c < 0 || d < 0) { cando_free(out); return NULL; }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)c <<  6) |  (uint32_t)d;
        out[j++] = (unsigned char)((v >> 16) & 0xFF);
        out[j++] = (unsigned char)((v >>  8) & 0xFF);
        out[j++] = (unsigned char)( v        & 0xFF);
    }
    if (rem == 2) {
        int a = b64_decode_value(in[full * 4],     urlsafe);
        int b = b64_decode_value(in[full * 4 + 1], urlsafe);
        if (a < 0 || b < 0) { cando_free(out); return NULL; }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        out[j++] = (unsigned char)((v >> 16) & 0xFF);
    } else if (rem == 3) {
        int a = b64_decode_value(in[full * 4],     urlsafe);
        int b = b64_decode_value(in[full * 4 + 1], urlsafe);
        int c = b64_decode_value(in[full * 4 + 2], urlsafe);
        if (a < 0 || b < 0 || c < 0) { cando_free(out); return NULL; }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6);
        out[j++] = (unsigned char)((v >> 16) & 0xFF);
        out[j++] = (unsigned char)((v >>  8) & 0xFF);
    }
    if (out_len) *out_len = j;
    return out;
}

/* Push bytes onto the VM stack encoded as requested.  Frees no
 * memory; caller still owns `bytes`. */
static void push_encoded(CandoVM *vm, const unsigned char *bytes,
                         size_t len, CryptoEnc enc)
{
    switch (enc) {
        case CRYPTO_ENC_HEX: {
            /* Cap stack-allocated hex buffer to a sane size; fall back
             * to heap for very long inputs (e.g. RSA signatures). */
            if (len <= 256) {
                char buf[513];
                hex_encode_into(bytes, len, buf);
                libutil_push_str(vm, buf, (u32)(len * 2));
            } else {
                char *buf = (char *)cando_alloc(len * 2 + 1);
                hex_encode_into(bytes, len, buf);
                libutil_push_str(vm, buf, (u32)(len * 2));
                cando_free(buf);
            }
            break;
        }
        case CRYPTO_ENC_BASE64: {
            size_t n;
            char *out = b64_encode_dup(bytes, len, false, &n);
            libutil_push_str(vm, out, (u32)n);
            cando_free(out);
            break;
        }
        case CRYPTO_ENC_BASE64URL: {
            size_t n;
            char *out = b64_encode_dup(bytes, len, true, &n);
            libutil_push_str(vm, out, (u32)n);
            cando_free(out);
            break;
        }
        case CRYPTO_ENC_BYTES:
        default:
            libutil_push_str(vm, (const char *)bytes, (u32)len);
            break;
    }
}

/* =========================================================================
 * OpenSSL error helper
 * ======================================================================= */

static void throw_openssl(CandoVM *vm, const char *fn_name)
{
    /* Surface only the topmost error string -- the full stack is
     * usually noise to script callers. */
    unsigned long code = ERR_get_error();
    char buf[256];
    if (code) {
        ERR_error_string_n(code, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "unknown OpenSSL error");
    }
    ERR_clear_error();
    cando_vm_error(vm, "crypto.%s: %s", fn_name, buf);
}

/* =========================================================================
 * Object-field accessors for opts objects.  Mirrors the helpers in
 * modules/sqlite/sqlite_module.c so the patterns stay consistent.
 * ======================================================================= */

static bool obj_get_str(CdoObject *obj, const char *key,
                        const char **out_data, u32 *out_len)
{
    if (!obj) return false;
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_STRING) return false;
    *out_data = v.as.string->data;
    if (out_len) *out_len = v.as.string->length;
    return true;
}

static bool obj_get_num(CdoObject *obj, const char *key, f64 *out)
{
    if (!obj) return false;
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = v.as.number;
    return true;
}

static void obj_set_str(CdoObject *obj, const char *key,
                        const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static void obj_set_num(CdoObject *obj, const char *key, f64 v)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(v), FIELD_NONE);
    cdo_string_release(k);
}

/* =========================================================================
 * Hashing
 *
 * One generic dispatch + per-algorithm trampolines.  Each per-algorithm
 * native exists so script code can call `crypto.sha256(...)` directly
 * without the indirection of `crypto.hash("sha256", ...)`.
 * ======================================================================= */

static int do_hash(CandoVM *vm, const EVP_MD *md, const char *name,
                   const char *data, u32 len, CryptoEnc enc)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        cando_vm_error(vm, "crypto.%s: out of memory", name);
        return -1;
    }
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw_openssl(vm, name);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    push_encoded(vm, digest, digest_len, enc);
    return 1;
}

/* Resolve a digest algorithm by name.  Accepts both Node-style spellings
 * ("sha3-256") and stdlib-style ("sha3_256"); the EVP table accepts the
 * former so we normalise the underscore form first. */
static const EVP_MD *resolve_md(const char *name)
{
    if (!name) return NULL;
    /* Translate `_` -> `-` so "sha3_256" maps to OpenSSL's "sha3-256". */
    char tmp[64];
    size_t n = strlen(name);
    if (n >= sizeof(tmp)) return NULL;
    for (size_t i = 0; i < n; i++) tmp[i] = (name[i] == '_') ? '-' : name[i];
    tmp[n] = '\0';
    const EVP_MD *md = EVP_get_digestbyname(tmp);
    if (md) return md;
    /* Try a couple of common aliases. */
    if (strcmp(tmp, "blake2b") == 0)  return EVP_get_digestbyname("blake2b512");
    if (strcmp(tmp, "blake2s") == 0)  return EVP_get_digestbyname("blake2s256");
    return NULL;
}

static int hash_simple(CandoVM *vm, int argc, CandoValue *args,
                       const char *algo_name)
{
    const EVP_MD *md = resolve_md(algo_name);
    if (!md) {
        cando_vm_error(vm, "crypto.%s: unsupported algorithm", algo_name);
        return -1;
    }
    const char *data = "";
    u32 len = 0;
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (s) { data = s->data; len = s->length; }
    CryptoEnc enc = enc_from_args(args, argc, 1);
    return do_hash(vm, md, algo_name, data, len, enc);
}

static int crypto_md5(CandoVM *vm, int argc, CandoValue *args)       { return hash_simple(vm, argc, args, "md5"); }
static int crypto_sha1(CandoVM *vm, int argc, CandoValue *args)      { return hash_simple(vm, argc, args, "sha1"); }
static int crypto_sha224(CandoVM *vm, int argc, CandoValue *args)    { return hash_simple(vm, argc, args, "sha224"); }
static int crypto_sha256(CandoVM *vm, int argc, CandoValue *args)    { return hash_simple(vm, argc, args, "sha256"); }
static int crypto_sha384(CandoVM *vm, int argc, CandoValue *args)    { return hash_simple(vm, argc, args, "sha384"); }
static int crypto_sha512(CandoVM *vm, int argc, CandoValue *args)    { return hash_simple(vm, argc, args, "sha512"); }
static int crypto_sha3_224(CandoVM *vm, int argc, CandoValue *args)  { return hash_simple(vm, argc, args, "sha3-224"); }
static int crypto_sha3_256(CandoVM *vm, int argc, CandoValue *args)  { return hash_simple(vm, argc, args, "sha3-256"); }
static int crypto_sha3_384(CandoVM *vm, int argc, CandoValue *args)  { return hash_simple(vm, argc, args, "sha3-384"); }
static int crypto_sha3_512(CandoVM *vm, int argc, CandoValue *args)  { return hash_simple(vm, argc, args, "sha3-512"); }
static int crypto_blake2b(CandoVM *vm, int argc, CandoValue *args)   { return hash_simple(vm, argc, args, "blake2b512"); }
static int crypto_blake2s(CandoVM *vm, int argc, CandoValue *args)   { return hash_simple(vm, argc, args, "blake2s256"); }

/* crypto.hash(algo, data, enc?) -- dynamic algorithm selection. */
static int crypto_hash(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "crypto.hash: first argument must be the algorithm name");
        return -1;
    }
    const char *algo = cando_as_string(args[0])->data;
    const EVP_MD *md = resolve_md(algo);
    if (!md) {
        cando_vm_error(vm, "crypto.hash: unsupported algorithm '%s'", algo);
        return -1;
    }
    const char *data = "";
    u32 len = 0;
    CandoString *s = libutil_arg_str_at(args, argc, 1);
    if (s) { data = s->data; len = s->length; }
    CryptoEnc enc = enc_from_args(args, argc, 2);
    return do_hash(vm, md, algo, data, len, enc);
}

/* =========================================================================
 * HMAC
 * ======================================================================= */

static int crypto_hmac(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3 || !cando_is_string(args[0])
                 || !cando_is_string(args[1])
                 || !cando_is_string(args[2])) {
        cando_vm_error(vm, "crypto.hmac: expected (algo, key, data, encoding?)");
        return -1;
    }
    const char *algo = cando_as_string(args[0])->data;
    CandoString *key  = cando_as_string(args[1]);
    CandoString *data = cando_as_string(args[2]);
    CryptoEnc enc = enc_from_args(args, argc, 3);

    const EVP_MD *md = resolve_md(algo);
    if (!md) {
        cando_vm_error(vm, "crypto.hmac: unsupported algorithm '%s'", algo);
        return -1;
    }

    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  out_len = 0;
    if (!HMAC(md, key->data, (int)key->length,
              (const unsigned char *)data->data, data->length,
              out, &out_len)) {
        throw_openssl(vm, "hmac");
        return -1;
    }
    push_encoded(vm, out, out_len, enc);
    return 1;
}

/* =========================================================================
 * KDFs: PBKDF2, scrypt, HKDF
 * ======================================================================= */

static int crypto_pbkdf2(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 5
        || !cando_is_string(args[0])
        || !cando_is_string(args[1])
        || !cando_is_number(args[2])
        || !cando_is_number(args[3])
        || !cando_is_string(args[4])) {
        cando_vm_error(vm,
            "crypto.pbkdf2: expected (password, salt, iterations, keylen, digest, encoding?)");
        return -1;
    }
    CandoString *pw   = cando_as_string(args[0]);
    CandoString *salt = cando_as_string(args[1]);
    int    iters  = (int)cando_as_number(args[2]);
    size_t keylen = (size_t)cando_as_number(args[3]);
    const char  *digest = cando_as_string(args[4])->data;
    CryptoEnc enc = enc_from_args_or(args, argc, 5, CRYPTO_ENC_BYTES);

    if (iters <= 0 || keylen == 0 || keylen > (1u << 24)) {
        cando_vm_error(vm, "crypto.pbkdf2: iterations and keylen must be positive (keylen <= 16M)");
        return -1;
    }
    const EVP_MD *md = resolve_md(digest);
    if (!md) {
        cando_vm_error(vm, "crypto.pbkdf2: unsupported digest '%s'", digest);
        return -1;
    }
    unsigned char *out = (unsigned char *)cando_alloc(keylen);
    if (PKCS5_PBKDF2_HMAC(pw->data, (int)pw->length,
                          (const unsigned char *)salt->data, (int)salt->length,
                          iters, md, (int)keylen, out) != 1) {
        cando_free(out);
        throw_openssl(vm, "pbkdf2");
        return -1;
    }
    push_encoded(vm, out, keylen, enc);
    cando_free(out);
    return 1;
}

static int crypto_scrypt(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3
        || !cando_is_string(args[0])
        || !cando_is_string(args[1])
        || !cando_is_number(args[2])) {
        cando_vm_error(vm,
            "crypto.scrypt: expected (password, salt, keylen, N?, r?, p?, encoding?)");
        return -1;
    }
    CandoString *pw   = cando_as_string(args[0]);
    CandoString *salt = cando_as_string(args[1]);
    size_t keylen = (size_t)cando_as_number(args[2]);
    uint64_t N = (argc >= 4 && cando_is_number(args[3])) ? (uint64_t)cando_as_number(args[3]) : 16384;
    uint64_t r = (argc >= 5 && cando_is_number(args[4])) ? (uint64_t)cando_as_number(args[4]) : 8;
    uint64_t p = (argc >= 6 && cando_is_number(args[5])) ? (uint64_t)cando_as_number(args[5]) : 1;
    int enc_idx = -1;
    /* Encoding lives after the optional N,r,p; find it as the last string arg. */
    for (int i = argc - 1; i >= 3; i--) {
        if (cando_is_string(args[i])) { enc_idx = i; break; }
    }
    CryptoEnc enc = (enc_idx > 0) ? parse_encoding(cando_as_string(args[enc_idx])->data) : CRYPTO_ENC_BYTES;

    if (keylen == 0 || keylen > (1u << 24)) {
        cando_vm_error(vm, "crypto.scrypt: keylen must be in (0, 16M]");
        return -1;
    }

    unsigned char *out = (unsigned char *)cando_alloc(keylen);
    /* Give scrypt enough memory headroom for typical N=16384, r=8 (16MB);
     * EVP_PBE_scrypt enforces its own check too. */
    if (EVP_PBE_scrypt(pw->data, pw->length,
                       (const unsigned char *)salt->data, salt->length,
                       N, r, p, 1024ULL * 1024ULL * 256ULL,
                       out, keylen) <= 0) {
        cando_free(out);
        throw_openssl(vm, "scrypt");
        return -1;
    }
    push_encoded(vm, out, keylen, enc);
    cando_free(out);
    return 1;
}

static int crypto_hkdf(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 5
        || !cando_is_string(args[0])
        || !cando_is_string(args[1])
        || !cando_is_string(args[2])
        || !cando_is_string(args[3])
        || !cando_is_number(args[4])) {
        cando_vm_error(vm,
            "crypto.hkdf: expected (digest, ikm, salt, info, keylen, encoding?)");
        return -1;
    }
    const char *digest = cando_as_string(args[0])->data;
    CandoString *ikm  = cando_as_string(args[1]);
    CandoString *salt = cando_as_string(args[2]);
    CandoString *info = cando_as_string(args[3]);
    size_t keylen = (size_t)cando_as_number(args[4]);
    CryptoEnc enc = enc_from_args_or(args, argc, 5, CRYPTO_ENC_BYTES);

    const EVP_MD *md = resolve_md(digest);
    if (!md) {
        cando_vm_error(vm, "crypto.hkdf: unsupported digest '%s'", digest);
        return -1;
    }
    if (keylen == 0 || keylen > (1u << 20)) {
        cando_vm_error(vm, "crypto.hkdf: keylen must be in (0, 1M]");
        return -1;
    }

    EVP_KDF      *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX  *kctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    if (!kctx) {
        if (kdf) EVP_KDF_free(kdf);
        throw_openssl(vm, "hkdf");
        return -1;
    }

    OSSL_PARAM params[5];
    int p = 0;
    params[p++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                   (char *)EVP_MD_get0_name(md), 0);
    params[p++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                                    (void *)ikm->data, ikm->length);
    if (salt->length > 0) {
        params[p++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                        (void *)salt->data, salt->length);
    }
    if (info->length > 0) {
        params[p++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                                        (void *)info->data, info->length);
    }
    params[p] = OSSL_PARAM_construct_end();

    unsigned char *out = (unsigned char *)cando_alloc(keylen);
    int rc = EVP_KDF_derive(kctx, out, keylen, params);
    EVP_KDF_CTX_free(kctx);
    EVP_KDF_free(kdf);
    if (rc <= 0) {
        cando_free(out);
        throw_openssl(vm, "hkdf");
        return -1;
    }
    push_encoded(vm, out, keylen, enc);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * Random
 * ======================================================================= */

static int crypto_random_bytes(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_number(args[0])) {
        cando_vm_error(vm, "crypto.randomBytes: expected (n, encoding?)");
        return -1;
    }
    size_t n = (size_t)cando_as_number(args[0]);
    if (n == 0) {
        push_encoded(vm, (const unsigned char *)"", 0,
                     enc_from_args(args, argc, 1));
        return 1;
    }
    if (n > (1u << 24)) {
        cando_vm_error(vm, "crypto.randomBytes: requested too many bytes (max 16M)");
        return -1;
    }
    /* Default encoding for randomBytes is "bytes" -- callers usually
     * want raw bytes for keys / IVs.  Pass an explicit "hex" if you
     * want hex. */
    CryptoEnc enc = (argc >= 2 && cando_is_string(args[1]))
        ? parse_encoding(cando_as_string(args[1])->data)
        : CRYPTO_ENC_BYTES;
    unsigned char *buf = (unsigned char *)cando_alloc(n);
    if (RAND_bytes(buf, (int)n) != 1) {
        cando_free(buf);
        throw_openssl(vm, "randomBytes");
        return -1;
    }
    push_encoded(vm, buf, n, enc);
    cando_free(buf);
    return 1;
}

static int crypto_random_int(CandoVM *vm, int argc, CandoValue *args)
{
    int64_t min = 0, max = 0;
    if (argc == 1 && cando_is_number(args[0])) {
        max = (int64_t)cando_as_number(args[0]);
    } else if (argc >= 2 && cando_is_number(args[0]) && cando_is_number(args[1])) {
        min = (int64_t)cando_as_number(args[0]);
        max = (int64_t)cando_as_number(args[1]);
    } else {
        cando_vm_error(vm, "crypto.randomInt: expected (max) or (min, max)");
        return -1;
    }
    if (max <= min) {
        cando_vm_error(vm, "crypto.randomInt: max must be > min");
        return -1;
    }
    uint64_t range = (uint64_t)(max - min);
    /* Reject values from the top of the 64-bit space that would bias
     * the distribution.  Standard rejection-sampling. */
    uint64_t limit = UINT64_MAX - (UINT64_MAX % range);
    uint64_t r;
    do {
        if (RAND_bytes((unsigned char *)&r, sizeof(r)) != 1) {
            throw_openssl(vm, "randomInt");
            return -1;
        }
    } while (r >= limit);
    int64_t out = (int64_t)(min + (int64_t)(r % range));
    cando_vm_push(vm, cando_number((f64)out));
    return 1;
}

static int crypto_random_uuid(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    unsigned char b[16];
    if (RAND_bytes(b, 16) != 1) {
        throw_openssl(vm, "randomUUID");
        return -1;
    }
    /* RFC 4122 v4: set version (top of byte 6) and variant (top of byte 8). */
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    libutil_push_str(vm, buf, 36);
    return 1;
}

/* =========================================================================
 * Symmetric ciphers
 *
 * Algorithm name passes straight through to OpenSSL (e.g. "aes-256-gcm").
 * For AEAD modes the encrypt path returns { ciphertext, tag } as an
 * object; decrypt takes tag separately.  For non-AEAD modes both return
 * raw ciphertext/plaintext strings (or the requested encoding).
 *
 * IV / key lengths are validated against the cipher's declared values;
 * GCM accepts a non-standard IV length and we just pass it through to
 * EVP_CIPHER_CTX_ctrl.
 * ======================================================================= */

static bool cipher_is_aead(const EVP_CIPHER *c)
{
    int mode = EVP_CIPHER_get_mode(c);
    return mode == EVP_CIPH_GCM_MODE
        || mode == EVP_CIPH_CCM_MODE
        || mode == EVP_CIPH_OCB_MODE
        /* ChaCha20-Poly1305 reports mode 0 on some builds; flag check. */
        || (EVP_CIPHER_get_flags(c) & EVP_CIPH_FLAG_AEAD_CIPHER);
}

static int crypto_encrypt(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4
        || !cando_is_string(args[0])  /* algo */
        || !cando_is_string(args[1])  /* key  */
        || !cando_is_string(args[2])  /* iv   */
        || !cando_is_string(args[3])) /* data */
    {
        cando_vm_error(vm,
            "crypto.encrypt: expected (algo, key, iv, data, aad?, encoding?)");
        return -1;
    }
    const char *algo = cando_as_string(args[0])->data;
    CandoString *key  = cando_as_string(args[1]);
    CandoString *iv   = cando_as_string(args[2]);
    CandoString *data = cando_as_string(args[3]);
    CandoString *aad  = (argc >= 5 && cando_is_string(args[4])) ? cando_as_string(args[4]) : NULL;
    CryptoEnc enc = enc_from_args_or(args, argc, aad ? 5 : 4, CRYPTO_ENC_BYTES);

    const EVP_CIPHER *cipher = EVP_get_cipherbyname(algo);
    if (!cipher) {
        cando_vm_error(vm, "crypto.encrypt: unsupported cipher '%s'", algo);
        return -1;
    }
    bool aead = cipher_is_aead(cipher);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { throw_openssl(vm, "encrypt"); return -1; }

    /* Init without key/iv first so we can set an explicit IV length
     * for GCM modes that allow it. */
    if (EVP_EncryptInit_ex2(ctx, cipher, NULL, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "encrypt");
        return -1;
    }
    if (aead && iv->length != (u32)EVP_CIPHER_get_iv_length(cipher)) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv->length, NULL) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "encrypt");
            return -1;
        }
    }
    if (EVP_EncryptInit_ex2(ctx, NULL,
                            (const unsigned char *)key->data,
                            (const unsigned char *)iv->data, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "encrypt");
        return -1;
    }

    int outl = 0;
    if (aead && aad) {
        int unused = 0;
        if (EVP_EncryptUpdate(ctx, NULL, &unused,
                              (const unsigned char *)aad->data, (int)aad->length) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "encrypt");
            return -1;
        }
    }

    int block = EVP_CIPHER_get_block_size(cipher);
    if (block < 1) block = 1;
    size_t ct_cap = (size_t)data->length + (size_t)block + 32;
    unsigned char *ct = (unsigned char *)cando_alloc(ct_cap);
    int ct_len = 0;
    if (EVP_EncryptUpdate(ctx, ct, &outl,
                          (const unsigned char *)data->data, (int)data->length) != 1) {
        cando_free(ct); EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "encrypt"); return -1;
    }
    ct_len = outl;
    if (EVP_EncryptFinal_ex(ctx, ct + ct_len, &outl) != 1) {
        cando_free(ct); EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "encrypt"); return -1;
    }
    ct_len += outl;

    unsigned char tag[16];
    int tag_len = 0;
    if (aead) {
        tag_len = 16;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tag_len, tag) != 1) {
            cando_free(ct); EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "encrypt"); return -1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);

    if (aead) {
        /* Return { ciphertext, tag } encoded uniformly. */
        CandoValue obj_val = cando_bridge_new_object(vm);
        CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));

        /* Encode each into a script-visible CdoString. */
        size_t ct_enc_len; unsigned char ct_enc_buf_stack[1];
        (void)ct_enc_buf_stack;

        /* Use intermediate push/pop into the VM stack -- simpler than
         * duplicating the encoder.  Push, then pop into the object. */
        push_encoded(vm, ct, ct_len, enc);
        CandoValue ct_val = cando_vm_pop(vm);
        push_encoded(vm, tag, tag_len, enc);
        CandoValue tag_val = cando_vm_pop(vm);

        CdoString *k_ct  = cdo_string_intern("ciphertext", 10);
        CdoString *k_tag = cdo_string_intern("tag", 3);
        cdo_object_rawset(obj, k_ct,  cando_bridge_to_cdo(vm, ct_val),  FIELD_NONE);
        cdo_object_rawset(obj, k_tag, cando_bridge_to_cdo(vm, tag_val), FIELD_NONE);
        cdo_string_release(k_ct);
        cdo_string_release(k_tag);
        cando_value_release(ct_val);
        cando_value_release(tag_val);

        cando_free(ct);
        cando_vm_push(vm, obj_val);
        (void)ct_enc_len;
        return 1;
    }

    push_encoded(vm, ct, ct_len, enc);
    cando_free(ct);
    return 1;
}

static int crypto_decrypt(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 4
        || !cando_is_string(args[0])
        || !cando_is_string(args[1])
        || !cando_is_string(args[2])
        || !cando_is_string(args[3])) {
        cando_vm_error(vm,
            "crypto.decrypt: expected (algo, key, iv, ciphertext, aad?, tag?, encoding?)");
        return -1;
    }
    const char *algo = cando_as_string(args[0])->data;
    CandoString *key  = cando_as_string(args[1]);
    CandoString *iv   = cando_as_string(args[2]);
    CandoString *ct   = cando_as_string(args[3]);
    CandoString *aad  = (argc >= 5 && cando_is_string(args[4])) ? cando_as_string(args[4]) : NULL;
    CandoString *tag  = (argc >= 6 && cando_is_string(args[5])) ? cando_as_string(args[5]) : NULL;
    CryptoEnc enc = enc_from_args_or(args, argc, tag ? 6 : aad ? 5 : 4, CRYPTO_ENC_BYTES);

    const EVP_CIPHER *cipher = EVP_get_cipherbyname(algo);
    if (!cipher) {
        cando_vm_error(vm, "crypto.decrypt: unsupported cipher '%s'", algo);
        return -1;
    }
    bool aead = cipher_is_aead(cipher);
    if (aead && !tag) {
        cando_vm_error(vm, "crypto.decrypt: AEAD cipher '%s' requires a tag", algo);
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { throw_openssl(vm, "decrypt"); return -1; }

    if (EVP_DecryptInit_ex2(ctx, cipher, NULL, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "decrypt"); return -1;
    }
    if (aead && iv->length != (u32)EVP_CIPHER_get_iv_length(cipher)) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv->length, NULL) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "decrypt"); return -1;
        }
    }
    if (EVP_DecryptInit_ex2(ctx, NULL,
                            (const unsigned char *)key->data,
                            (const unsigned char *)iv->data, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "decrypt"); return -1;
    }

    int outl;
    if (aead && aad) {
        int unused = 0;
        if (EVP_DecryptUpdate(ctx, NULL, &unused,
                              (const unsigned char *)aad->data, (int)aad->length) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "decrypt"); return -1;
        }
    }

    int block = EVP_CIPHER_get_block_size(cipher);
    if (block < 1) block = 1;
    size_t pt_cap = (size_t)ct->length + (size_t)block + 32;
    unsigned char *pt = (unsigned char *)cando_alloc(pt_cap);
    int pt_len = 0;
    if (EVP_DecryptUpdate(ctx, pt, &outl,
                          (const unsigned char *)ct->data, (int)ct->length) != 1) {
        cando_free(pt); EVP_CIPHER_CTX_free(ctx);
        throw_openssl(vm, "decrypt"); return -1;
    }
    pt_len = outl;

    if (aead) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, (int)tag->length,
                                (void *)tag->data) != 1) {
            cando_free(pt); EVP_CIPHER_CTX_free(ctx);
            throw_openssl(vm, "decrypt"); return -1;
        }
    }
    int final_rc = EVP_DecryptFinal_ex(ctx, pt + pt_len, &outl);
    EVP_CIPHER_CTX_free(ctx);
    if (final_rc != 1) {
        cando_free(pt);
        cando_vm_error(vm,
            "crypto.decrypt: authentication failed (wrong tag/aad/key/iv?)");
        return -1;
    }
    pt_len += outl;

    push_encoded(vm, pt, pt_len, enc);
    cando_free(pt);
    return 1;
}

/* =========================================================================
 * PEM <-> EVP_PKEY helpers.
 *
 * Keys are passed as PEM strings in script-visible code; every routine
 * that needs an EVP_PKEY parses a PEM blob, uses it, and frees it.
 * ======================================================================= */

static EVP_PKEY *pkey_from_private_pem(const char *pem, size_t len)
{
    BIO *b = BIO_new_mem_buf(pem, (int)len);
    if (!b) return NULL;
    EVP_PKEY *k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    BIO_free(b);
    return k;
}

static EVP_PKEY *pkey_from_public_pem(const char *pem, size_t len)
{
    BIO *b = BIO_new_mem_buf(pem, (int)len);
    if (!b) return NULL;
    EVP_PKEY *k = PEM_read_bio_PUBKEY(b, NULL, NULL, NULL);
    BIO_free(b);
    return k;
}

static char *pem_from_pkey_private(EVP_PKEY *k, size_t *out_len)
{
    BIO *b = BIO_new(BIO_s_mem());
    if (!b) return NULL;
    if (PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(b); return NULL;
    }
    BUF_MEM *bm; BIO_get_mem_ptr(b, &bm);
    char *out = (char *)cando_alloc(bm->length + 1);
    memcpy(out, bm->data, bm->length);
    out[bm->length] = '\0';
    if (out_len) *out_len = bm->length;
    BIO_free(b);
    return out;
}

static char *pem_from_pkey_public(EVP_PKEY *k, size_t *out_len)
{
    BIO *b = BIO_new(BIO_s_mem());
    if (!b) return NULL;
    if (PEM_write_bio_PUBKEY(b, k) != 1) {
        BIO_free(b); return NULL;
    }
    BUF_MEM *bm; BIO_get_mem_ptr(b, &bm);
    char *out = (char *)cando_alloc(bm->length + 1);
    memcpy(out, bm->data, bm->length);
    out[bm->length] = '\0';
    if (out_len) *out_len = bm->length;
    BIO_free(b);
    return out;
}

/* =========================================================================
 * Asymmetric -- generateKeyPair
 *
 * Returns { publicKey, privateKey } as PEM strings.  Supports rsa, ec,
 * ed25519, x25519.
 * ======================================================================= */

static int crypto_generate_keypair(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "crypto.generateKeyPair: expected (type, opts?)");
        return -1;
    }
    const char *type = cando_as_string(args[0])->data;
    CdoObject *opts = NULL;
    if (argc >= 2 && cando_is_object(args[1])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[1]));
    }

    EVP_PKEY_CTX *kctx = NULL;
    EVP_PKEY     *pkey = NULL;

    if (strcmp(type, "rsa") == 0) {
        f64 bits_d = 2048;
        if (opts) obj_get_num(opts, "modulusLength", &bits_d);
        int bits = (int)bits_d;
        if (bits < 1024 || bits > 16384) {
            cando_vm_error(vm,
                "crypto.generateKeyPair(rsa): modulusLength must be in [1024, 16384]");
            return -1;
        }
        kctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
                 || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, bits) <= 0
                 || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(kctx);
            throw_openssl(vm, "generateKeyPair");
            return -1;
        }
    } else if (strcmp(type, "ec") == 0) {
        const char *curve = "P-256";
        u32 unused;
        if (opts) obj_get_str(opts, "curve", &curve, &unused);
        /* Map common spellings to OpenSSL group names. */
        const char *grp = curve;
        if      (strcmp(curve, "P-256") == 0)  grp = "prime256v1";
        else if (strcmp(curve, "P-384") == 0)  grp = "secp384r1";
        else if (strcmp(curve, "P-521") == 0)  grp = "secp521r1";
        else if (strcmp(curve, "secp256k1") == 0) grp = "secp256k1";

        kctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (!kctx
            || EVP_PKEY_keygen_init(kctx) <= 0
            || EVP_PKEY_CTX_set_group_name(kctx, grp) <= 0
            || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(kctx);
            cando_vm_error(vm,
                "crypto.generateKeyPair(ec): bad curve '%s' or keygen failed", curve);
            return -1;
        }
    } else if (strcmp(type, "ed25519") == 0) {
        kctx = EVP_PKEY_CTX_new_from_name(NULL, "ED25519", NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
                 || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(kctx);
            throw_openssl(vm, "generateKeyPair");
            return -1;
        }
    } else if (strcmp(type, "x25519") == 0) {
        kctx = EVP_PKEY_CTX_new_from_name(NULL, "X25519", NULL);
        if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
                 || EVP_PKEY_keygen(kctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(kctx);
            throw_openssl(vm, "generateKeyPair");
            return -1;
        }
    } else {
        cando_vm_error(vm,
            "crypto.generateKeyPair: unsupported type '%s' "
            "(expected one of rsa, ec, ed25519, x25519)", type);
        return -1;
    }
    EVP_PKEY_CTX_free(kctx);

    size_t pub_len = 0, prv_len = 0;
    char *pub_pem = pem_from_pkey_public(pkey, &pub_len);
    char *prv_pem = pem_from_pkey_private(pkey, &prv_len);
    EVP_PKEY_free(pkey);

    if (!pub_pem || !prv_pem) {
        cando_free(pub_pem); cando_free(prv_pem);
        cando_vm_error(vm, "crypto.generateKeyPair: PEM serialisation failed");
        return -1;
    }

    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    obj_set_str(obj, "publicKey",  pub_pem, (u32)pub_len);
    obj_set_str(obj, "privateKey", prv_pem, (u32)prv_len);
    cando_free(pub_pem);
    cando_free(prv_pem);

    cando_vm_push(vm, obj_val);
    return 1;
}

/* =========================================================================
 * Asymmetric -- sign / verify
 *
 * Defaults: digest = sha256 (ignored for Ed25519).  RSA padding =
 * PKCS#1 v1.5; "pss" available via opts.padding.
 * ======================================================================= */

static bool pkey_is_eddsa(EVP_PKEY *k)
{
    int id = EVP_PKEY_get_id(k);
    return id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448;
}

static int crypto_sign(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm,
            "crypto.sign: expected (privateKeyPem, data, opts?, encoding?)");
        return -1;
    }
    CandoString *pem  = cando_as_string(args[0]);
    CandoString *data = cando_as_string(args[1]);
    CdoObject *opts = NULL;
    int enc_idx = -1;
    for (int i = argc - 1; i >= 2; i--) {
        if (cando_is_string(args[i])) { enc_idx = i; break; }
    }
    if (argc >= 3 && cando_is_object(args[2])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[2]));
    }
    CryptoEnc enc = (enc_idx >= 2) ? parse_encoding(cando_as_string(args[enc_idx])->data) : CRYPTO_ENC_BYTES;

    EVP_PKEY *key = pkey_from_private_pem(pem->data, pem->length);
    if (!key) { cando_vm_error(vm, "crypto.sign: failed to parse private key PEM"); return -1; }

    const char *digest_name = "sha256";
    const char *padding = "pkcs1";
    f64 salt_len_d = -1;
    u32 unused;
    if (opts) {
        obj_get_str(opts, "digest", &digest_name, &unused);
        obj_get_str(opts, "padding", &padding, &unused);
        obj_get_num(opts, "saltLength", &salt_len_d);
    }

    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    if (!mctx) { EVP_PKEY_free(key); throw_openssl(vm, "sign"); return -1; }

    const EVP_MD *md = pkey_is_eddsa(key) ? NULL : resolve_md(digest_name);
    if (!pkey_is_eddsa(key) && !md) {
        EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
        cando_vm_error(vm, "crypto.sign: unsupported digest '%s'", digest_name);
        return -1;
    }
    EVP_PKEY_CTX *pctx = NULL;
    if (EVP_DigestSignInit(mctx, &pctx, md, NULL, key) != 1) {
        EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
        throw_openssl(vm, "sign"); return -1;
    }

    if (EVP_PKEY_get_id(key) == EVP_PKEY_RSA && strcmp(padding, "pss") == 0) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) {
            EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "sign"); return -1;
        }
        if (salt_len_d >= 0) {
            if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, (int)salt_len_d) <= 0) {
                EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
                throw_openssl(vm, "sign"); return -1;
            }
        }
    }

    size_t sig_len = 0;
    if (pkey_is_eddsa(key)) {
        if (EVP_DigestSign(mctx, NULL, &sig_len,
                           (const unsigned char *)data->data, data->length) != 1) {
            EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "sign"); return -1;
        }
        unsigned char *sig = (unsigned char *)cando_alloc(sig_len);
        if (EVP_DigestSign(mctx, sig, &sig_len,
                           (const unsigned char *)data->data, data->length) != 1) {
            cando_free(sig); EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "sign"); return -1;
        }
        push_encoded(vm, sig, sig_len, enc);
        cando_free(sig);
    } else {
        if (EVP_DigestSignUpdate(mctx, data->data, data->length) != 1
            || EVP_DigestSignFinal(mctx, NULL, &sig_len) != 1) {
            EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "sign"); return -1;
        }
        unsigned char *sig = (unsigned char *)cando_alloc(sig_len);
        if (EVP_DigestSignFinal(mctx, sig, &sig_len) != 1) {
            cando_free(sig); EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "sign"); return -1;
        }
        push_encoded(vm, sig, sig_len, enc);
        cando_free(sig);
    }
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(key);
    return 1;
}

static int crypto_verify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3 || !cando_is_string(args[0])
                 || !cando_is_string(args[1])
                 || !cando_is_string(args[2])) {
        cando_vm_error(vm,
            "crypto.verify: expected (publicKeyPem, data, signature, opts?)");
        return -1;
    }
    CandoString *pem  = cando_as_string(args[0]);
    CandoString *data = cando_as_string(args[1]);
    CandoString *sig  = cando_as_string(args[2]);
    CdoObject *opts = NULL;
    if (argc >= 4 && cando_is_object(args[3])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[3]));
    }
    EVP_PKEY *key = pkey_from_public_pem(pem->data, pem->length);
    if (!key) {
        cando_vm_error(vm, "crypto.verify: failed to parse public key PEM");
        return -1;
    }

    const char *digest_name = "sha256";
    const char *padding     = "pkcs1";
    f64 salt_len_d = -1;
    u32 unused;
    if (opts) {
        obj_get_str(opts, "digest", &digest_name, &unused);
        obj_get_str(opts, "padding", &padding, &unused);
        obj_get_num(opts, "saltLength", &salt_len_d);
    }

    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    if (!mctx) { EVP_PKEY_free(key); throw_openssl(vm, "verify"); return -1; }
    const EVP_MD *md = pkey_is_eddsa(key) ? NULL : resolve_md(digest_name);
    if (!pkey_is_eddsa(key) && !md) {
        EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
        cando_vm_error(vm, "crypto.verify: unsupported digest '%s'", digest_name);
        return -1;
    }
    EVP_PKEY_CTX *pctx = NULL;
    if (EVP_DigestVerifyInit(mctx, &pctx, md, NULL, key) != 1) {
        EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
        throw_openssl(vm, "verify"); return -1;
    }
    if (EVP_PKEY_get_id(key) == EVP_PKEY_RSA && strcmp(padding, "pss") == 0) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) {
            EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "verify"); return -1;
        }
        if (salt_len_d >= 0) {
            EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, (int)salt_len_d);
        }
    }

    int rc;
    if (pkey_is_eddsa(key)) {
        rc = EVP_DigestVerify(mctx,
                              (const unsigned char *)sig->data, sig->length,
                              (const unsigned char *)data->data, data->length);
    } else {
        if (EVP_DigestVerifyUpdate(mctx, data->data, data->length) != 1) {
            EVP_MD_CTX_free(mctx); EVP_PKEY_free(key);
            throw_openssl(vm, "verify"); return -1;
        }
        rc = EVP_DigestVerifyFinal(mctx,
                                   (const unsigned char *)sig->data,
                                   sig->length);
    }
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(key);

    /* OpenSSL: 1 = valid, 0 = invalid, <0 = error.  Treat any non-1
     * as `false` so script callers don't have to distinguish. */
    cando_vm_push(vm, cando_bool(rc == 1));
    /* Suppress the queued error for the false case. */
    if (rc != 1) ERR_clear_error();
    return 1;
}

/* =========================================================================
 * Asymmetric -- publicEncrypt / privateDecrypt (RSA only)
 * ======================================================================= */

static int rsa_padding_from_str(const char *p)
{
    if (!p) return RSA_PKCS1_OAEP_PADDING;
    if (strcmp(p, "oaep")  == 0) return RSA_PKCS1_OAEP_PADDING;
    if (strcmp(p, "pkcs1") == 0) return RSA_PKCS1_PADDING;
    if (strcmp(p, "none")  == 0) return RSA_NO_PADDING;
    return -1;
}

static int crypto_public_encrypt(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm,
            "crypto.publicEncrypt: expected (publicKeyPem, data, opts?, encoding?)");
        return -1;
    }
    CandoString *pem  = cando_as_string(args[0]);
    CandoString *data = cando_as_string(args[1]);
    CdoObject *opts = NULL;
    int enc_idx = -1;
    for (int i = argc - 1; i >= 2; i--) {
        if (cando_is_string(args[i])) { enc_idx = i; break; }
    }
    if (argc >= 3 && cando_is_object(args[2])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[2]));
    }
    CryptoEnc enc = (enc_idx >= 2) ? parse_encoding(cando_as_string(args[enc_idx])->data) : CRYPTO_ENC_BYTES;

    EVP_PKEY *key = pkey_from_public_pem(pem->data, pem->length);
    if (!key) { cando_vm_error(vm, "crypto.publicEncrypt: bad public key PEM"); return -1; }

    const char *pad = "oaep";
    u32 unused;
    if (opts) obj_get_str(opts, "padding", &pad, &unused);
    int pkpad = rsa_padding_from_str(pad);
    if (pkpad < 0) {
        EVP_PKEY_free(key);
        cando_vm_error(vm, "crypto.publicEncrypt: unknown padding '%s'", pad);
        return -1;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
    if (!ctx || EVP_PKEY_encrypt_init(ctx) <= 0
             || EVP_PKEY_CTX_set_rsa_padding(ctx, pkpad) <= 0) {
        if (ctx) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key); throw_openssl(vm, "publicEncrypt"); return -1;
    }

    size_t out_len = 0;
    if (EVP_PKEY_encrypt(ctx, NULL, &out_len,
                         (const unsigned char *)data->data, data->length) <= 0) {
        EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);
        throw_openssl(vm, "publicEncrypt"); return -1;
    }
    unsigned char *out = (unsigned char *)cando_alloc(out_len);
    if (EVP_PKEY_encrypt(ctx, out, &out_len,
                         (const unsigned char *)data->data, data->length) <= 0) {
        cando_free(out); EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);
        throw_openssl(vm, "publicEncrypt"); return -1;
    }
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);

    push_encoded(vm, out, out_len, enc);
    cando_free(out);
    return 1;
}

static int crypto_private_decrypt(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm,
            "crypto.privateDecrypt: expected (privateKeyPem, data, opts?, encoding?)");
        return -1;
    }
    CandoString *pem  = cando_as_string(args[0]);
    CandoString *data = cando_as_string(args[1]);
    CdoObject *opts = NULL;
    int enc_idx = -1;
    for (int i = argc - 1; i >= 2; i--) {
        if (cando_is_string(args[i])) { enc_idx = i; break; }
    }
    if (argc >= 3 && cando_is_object(args[2])) {
        opts = cando_bridge_resolve(vm, cando_as_handle(args[2]));
    }
    CryptoEnc enc = (enc_idx >= 2) ? parse_encoding(cando_as_string(args[enc_idx])->data) : CRYPTO_ENC_BYTES;

    EVP_PKEY *key = pkey_from_private_pem(pem->data, pem->length);
    if (!key) { cando_vm_error(vm, "crypto.privateDecrypt: bad private key PEM"); return -1; }

    const char *pad = "oaep";
    u32 unused;
    if (opts) obj_get_str(opts, "padding", &pad, &unused);
    int pkpad = rsa_padding_from_str(pad);
    if (pkpad < 0) {
        EVP_PKEY_free(key);
        cando_vm_error(vm, "crypto.privateDecrypt: unknown padding '%s'", pad);
        return -1;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
    if (!ctx || EVP_PKEY_decrypt_init(ctx) <= 0
             || EVP_PKEY_CTX_set_rsa_padding(ctx, pkpad) <= 0) {
        if (ctx) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(key); throw_openssl(vm, "privateDecrypt"); return -1;
    }

    size_t out_len = 0;
    if (EVP_PKEY_decrypt(ctx, NULL, &out_len,
                         (const unsigned char *)data->data, data->length) <= 0) {
        EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);
        throw_openssl(vm, "privateDecrypt"); return -1;
    }
    unsigned char *out = (unsigned char *)cando_alloc(out_len);
    if (EVP_PKEY_decrypt(ctx, out, &out_len,
                         (const unsigned char *)data->data, data->length) <= 0) {
        cando_free(out); EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);
        throw_openssl(vm, "privateDecrypt"); return -1;
    }
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(key);

    push_encoded(vm, out, out_len, enc);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * Diffie-Hellman (X25519 / ECDH)
 * ======================================================================= */

static int crypto_diffie_hellman(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm,
            "crypto.diffieHellman: expected (privateKeyPem, peerPublicKeyPem, encoding?)");
        return -1;
    }
    CandoString *priv = cando_as_string(args[0]);
    CandoString *peer = cando_as_string(args[1]);
    CryptoEnc enc = enc_from_args_or(args, argc, 2, CRYPTO_ENC_BYTES);

    EVP_PKEY *p_priv = pkey_from_private_pem(priv->data, priv->length);
    EVP_PKEY *p_pub  = pkey_from_public_pem(peer->data, peer->length);
    if (!p_priv || !p_pub) {
        if (p_priv) EVP_PKEY_free(p_priv);
        if (p_pub)  EVP_PKEY_free(p_pub);
        cando_vm_error(vm, "crypto.diffieHellman: failed to parse key PEM");
        return -1;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(p_priv, NULL);
    if (!ctx || EVP_PKEY_derive_init(ctx) <= 0
             || EVP_PKEY_derive_set_peer(ctx, p_pub) <= 0) {
        if (ctx) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(p_priv); EVP_PKEY_free(p_pub);
        throw_openssl(vm, "diffieHellman"); return -1;
    }
    size_t out_len = 0;
    if (EVP_PKEY_derive(ctx, NULL, &out_len) <= 0) {
        EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(p_priv); EVP_PKEY_free(p_pub);
        throw_openssl(vm, "diffieHellman"); return -1;
    }
    unsigned char *out = (unsigned char *)cando_alloc(out_len);
    if (EVP_PKEY_derive(ctx, out, &out_len) <= 0) {
        cando_free(out); EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(p_priv); EVP_PKEY_free(p_pub);
        throw_openssl(vm, "diffieHellman"); return -1;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(p_priv);
    EVP_PKEY_free(p_pub);

    push_encoded(vm, out, out_len, enc);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * X.509 -- create / parse / verify / fingerprint / csr
 *
 * Lives under crypto.x509.* in the registration table.
 * ======================================================================= */

/* Build an X509_NAME from a script { CN, O, OU, C, ST, L, emailAddress } object. */
static X509_NAME *name_from_obj(CdoObject *obj)
{
    if (!obj) return NULL;
    X509_NAME *nm = X509_NAME_new();
    if (!nm) return NULL;
    static const char *fields[] = {
        "C", "ST", "L", "O", "OU", "CN", "emailAddress", NULL
    };
    for (int i = 0; fields[i]; i++) {
        const char *data; u32 len;
        if (obj_get_str(obj, fields[i], &data, &len) && len > 0) {
            X509_NAME_add_entry_by_txt(nm, fields[i], MBSTRING_UTF8,
                                        (const unsigned char *)data,
                                        (int)len, -1, 0);
        }
    }
    return nm;
}

/* Convert an X509_NAME to a { CN, O, ... } CdoObject. */
static CandoValue obj_from_name(CandoVM *vm, X509_NAME *nm)
{
    CandoValue v = cando_bridge_new_object(vm);
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    int n = X509_NAME_entry_count(nm);
    for (int i = 0; i < n; i++) {
        X509_NAME_ENTRY *e = X509_NAME_get_entry(nm, i);
        ASN1_OBJECT     *oid = X509_NAME_ENTRY_get_object(e);
        ASN1_STRING     *as  = X509_NAME_ENTRY_get_data(e);
        char buf[80];
        OBJ_obj2txt(buf, sizeof(buf), oid, 0);
        /* Translate common OIDs to their short names. */
        if      (strcmp(buf, "commonName") == 0)             strcpy(buf, "CN");
        else if (strcmp(buf, "countryName") == 0)            strcpy(buf, "C");
        else if (strcmp(buf, "stateOrProvinceName") == 0)    strcpy(buf, "ST");
        else if (strcmp(buf, "localityName") == 0)           strcpy(buf, "L");
        else if (strcmp(buf, "organizationName") == 0)       strcpy(buf, "O");
        else if (strcmp(buf, "organizationalUnitName") == 0) strcpy(buf, "OU");
        unsigned char *data = NULL;
        int data_len = ASN1_STRING_to_UTF8(&data, as);
        if (data_len >= 0) {
            obj_set_str(o, buf, (const char *)data, (u32)data_len);
            OPENSSL_free(data);
        }
    }
    return v;
}

/* Convert an ASN1_TIME to a Unix-epoch number. */
static f64 asn1_time_to_unix(const ASN1_TIME *t)
{
    struct tm tm = {0};
    ASN1_TIME_to_tm(t, &tm);
    return (f64)timegm(&tm);
}

static int crypto_x509_create(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_object(args[0]) || !cando_is_object(args[1])) {
        cando_vm_error(vm,
            "crypto.x509.create: expected (subject, opts) where opts has "
            "privateKey, optional publicKey, days, serial, issuer, digest");
        return -1;
    }
    CdoObject *subj_obj = cando_bridge_resolve(vm, cando_as_handle(args[0]));
    CdoObject *opts     = cando_bridge_resolve(vm, cando_as_handle(args[1]));

    const char *priv_pem_data = NULL; u32 priv_pem_len = 0;
    const char *pub_pem_data  = NULL; u32 pub_pem_len  = 0;
    const char *digest_name = "sha256";
    f64 days = 365.0;
    f64 serial = 1.0;
    CdoObject *issuer_obj = NULL;
    u32 _u;

    if (!obj_get_str(opts, "privateKey", &priv_pem_data, &priv_pem_len)) {
        cando_vm_error(vm, "crypto.x509.create: opts.privateKey is required");
        return -1;
    }
    obj_get_str(opts, "publicKey", &pub_pem_data, &pub_pem_len);
    obj_get_str(opts, "digest", &digest_name, &_u);
    obj_get_num(opts, "days", &days);
    obj_get_num(opts, "serial", &serial);
    {
        CdoString *k = cdo_string_intern("issuer", 6);
        CdoValue v;
        bool ok = cdo_object_rawget(opts, k, &v);
        cdo_string_release(k);
        if (ok && v.tag == CDO_OBJECT) {
            issuer_obj = v.as.object;
        }
    }

    EVP_PKEY *priv = pkey_from_private_pem(priv_pem_data, priv_pem_len);
    if (!priv) { cando_vm_error(vm, "crypto.x509.create: bad private key PEM"); return -1; }
    EVP_PKEY *pub = pub_pem_data
        ? pkey_from_public_pem(pub_pem_data, pub_pem_len)
        : NULL;
    /* If no public key provided, self-sign: pub == priv. */
    EVP_PKEY *cert_pub = pub ? pub : priv;

    X509 *crt = X509_new();
    if (!crt) {
        EVP_PKEY_free(priv); if (pub) EVP_PKEY_free(pub);
        throw_openssl(vm, "x509.create"); return -1;
    }
    X509_set_version(crt, 2 /* v3 */);
    ASN1_INTEGER_set(X509_get_serialNumber(crt), (long)serial);
    X509_gmtime_adj(X509_get_notBefore(crt), 0);
    X509_gmtime_adj(X509_get_notAfter(crt), (long)(60 * 60 * 24 * days));
    X509_set_pubkey(crt, cert_pub);

    X509_NAME *snm = name_from_obj(subj_obj);
    X509_NAME *inm = issuer_obj ? name_from_obj(issuer_obj) : NULL;
    if (snm) X509_set_subject_name(crt, snm);
    X509_set_issuer_name(crt, inm ? inm : snm);

    const EVP_MD *md = resolve_md(digest_name);
    if (!md) {
        X509_free(crt);
        if (snm) X509_NAME_free(snm);
        if (inm) X509_NAME_free(inm);
        EVP_PKEY_free(priv); if (pub) EVP_PKEY_free(pub);
        cando_vm_error(vm, "crypto.x509.create: unsupported digest '%s'", digest_name);
        return -1;
    }
    if (X509_sign(crt, priv, md) == 0) {
        X509_free(crt);
        if (snm) X509_NAME_free(snm);
        if (inm) X509_NAME_free(inm);
        EVP_PKEY_free(priv); if (pub) EVP_PKEY_free(pub);
        throw_openssl(vm, "x509.create"); return -1;
    }
    if (snm) X509_NAME_free(snm);
    if (inm) X509_NAME_free(inm);

    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, crt);
    BUF_MEM *bm; BIO_get_mem_ptr(bio, &bm);
    libutil_push_str(vm, bm->data, (u32)bm->length);
    BIO_free(bio);
    X509_free(crt);
    EVP_PKEY_free(priv); if (pub) EVP_PKEY_free(pub);
    return 1;
}

static int crypto_x509_parse(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "crypto.x509.parse: expected (pem)");
        return -1;
    }
    CandoString *pem = cando_as_string(args[0]);
    BIO *b = BIO_new_mem_buf(pem->data, (int)pem->length);
    if (!b) { throw_openssl(vm, "x509.parse"); return -1; }
    X509 *crt = PEM_read_bio_X509(b, NULL, NULL, NULL);
    BIO_free(b);
    if (!crt) { cando_vm_error(vm, "crypto.x509.parse: not a PEM-encoded X.509 certificate"); return -1; }

    CandoValue out_val = cando_bridge_new_object(vm);
    CdoObject *out = cando_bridge_resolve(vm, cando_as_handle(out_val));

    CandoValue subj = obj_from_name(vm, X509_get_subject_name(crt));
    CandoValue iss  = obj_from_name(vm, X509_get_issuer_name(crt));
    {
        CdoString *k = cdo_string_intern("subject", 7);
        cdo_object_rawset(out, k, cando_bridge_to_cdo(vm, subj), FIELD_NONE);
        cdo_string_release(k);
        cando_value_release(subj);
    }
    {
        CdoString *k = cdo_string_intern("issuer", 6);
        cdo_object_rawset(out, k, cando_bridge_to_cdo(vm, iss), FIELD_NONE);
        cdo_string_release(k);
        cando_value_release(iss);
    }

    obj_set_num(out, "notBefore", asn1_time_to_unix(X509_get0_notBefore(crt)));
    obj_set_num(out, "notAfter",  asn1_time_to_unix(X509_get0_notAfter(crt)));

    /* Serial as decimal string -- can exceed double precision. */
    {
        ASN1_INTEGER *sn = X509_get_serialNumber(crt);
        BIGNUM *bn = ASN1_INTEGER_to_BN(sn, NULL);
        if (bn) {
            char *dec = BN_bn2dec(bn);
            if (dec) {
                obj_set_str(out, "serialNumber", dec, (u32)strlen(dec));
                OPENSSL_free(dec);
            }
            BN_free(bn);
        }
    }

    /* Fingerprint via SHA-256. */
    unsigned char md[EVP_MAX_MD_SIZE]; unsigned int md_len;
    if (X509_digest(crt, EVP_sha256(), md, &md_len)) {
        char hex[EVP_MAX_MD_SIZE * 2 + 1];
        hex_encode_into(md, md_len, hex);
        obj_set_str(out, "fingerprint", hex, md_len * 2);
    }

    /* Public key in PEM form. */
    EVP_PKEY *pk = X509_get_pubkey(crt);
    if (pk) {
        size_t pn;
        char *pem_pub = pem_from_pkey_public(pk, &pn);
        if (pem_pub) {
            obj_set_str(out, "publicKey", pem_pub, (u32)pn);
            cando_free(pem_pub);
        }
        EVP_PKEY_free(pk);
    }

    X509_free(crt);
    cando_vm_push(vm, out_val);
    return 1;
}

static int crypto_x509_verify(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "crypto.x509.verify: expected (certPem, caPem)");
        return -1;
    }
    CandoString *cert_pem = cando_as_string(args[0]);
    CandoString *ca_pem   = cando_as_string(args[1]);

    BIO *b1 = BIO_new_mem_buf(cert_pem->data, (int)cert_pem->length);
    X509 *cert = b1 ? PEM_read_bio_X509(b1, NULL, NULL, NULL) : NULL;
    if (b1) BIO_free(b1);
    BIO *b2 = BIO_new_mem_buf(ca_pem->data, (int)ca_pem->length);
    X509 *ca = b2 ? PEM_read_bio_X509(b2, NULL, NULL, NULL) : NULL;
    if (b2) BIO_free(b2);
    if (!cert || !ca) {
        if (cert) X509_free(cert);
        if (ca)   X509_free(ca);
        cando_vm_error(vm, "crypto.x509.verify: failed to parse PEM input");
        return -1;
    }

    X509_STORE *store = X509_STORE_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, cert, NULL);
    int ok = X509_verify_cert(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(cert);
    X509_free(ca);

    cando_vm_push(vm, cando_bool(ok == 1));
    if (ok != 1) ERR_clear_error();
    return 1;
}

static int crypto_x509_fingerprint(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "crypto.x509.fingerprint: expected (pem, algo?)");
        return -1;
    }
    CandoString *pem = cando_as_string(args[0]);
    const char *algo = (argc >= 2 && cando_is_string(args[1]))
        ? cando_as_string(args[1])->data : "sha256";
    const EVP_MD *md = resolve_md(algo);
    if (!md) { cando_vm_error(vm, "crypto.x509.fingerprint: bad algo '%s'", algo); return -1; }
    BIO *b = BIO_new_mem_buf(pem->data, (int)pem->length);
    X509 *crt = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    if (b) BIO_free(b);
    if (!crt) { cando_vm_error(vm, "crypto.x509.fingerprint: bad PEM"); return -1; }

    unsigned char buf[EVP_MAX_MD_SIZE]; unsigned int len;
    if (!X509_digest(crt, md, buf, &len)) {
        X509_free(crt); throw_openssl(vm, "x509.fingerprint"); return -1;
    }
    X509_free(crt);
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_encode_into(buf, len, hex);
    libutil_push_str(vm, hex, len * 2);
    return 1;
}

static int crypto_x509_csr(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_object(args[1])) {
        cando_vm_error(vm,
            "crypto.x509.csr: expected (privateKeyPem, subjectObj)");
        return -1;
    }
    CandoString *pem = cando_as_string(args[0]);
    CdoObject *subj  = cando_bridge_resolve(vm, cando_as_handle(args[1]));

    EVP_PKEY *priv = pkey_from_private_pem(pem->data, pem->length);
    if (!priv) { cando_vm_error(vm, "crypto.x509.csr: bad private key PEM"); return -1; }

    X509_REQ *req = X509_REQ_new();
    X509_REQ_set_version(req, 0);
    X509_REQ_set_pubkey(req, priv);
    X509_NAME *nm = name_from_obj(subj);
    if (nm) X509_REQ_set_subject_name(req, nm);
    if (X509_REQ_sign(req, priv, EVP_sha256()) == 0) {
        if (nm) X509_NAME_free(nm);
        X509_REQ_free(req); EVP_PKEY_free(priv);
        throw_openssl(vm, "x509.csr"); return -1;
    }
    if (nm) X509_NAME_free(nm);
    BIO *b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509_REQ(b, req);
    BUF_MEM *bm; BIO_get_mem_ptr(b, &bm);
    libutil_push_str(vm, bm->data, (u32)bm->length);
    BIO_free(b);
    X509_REQ_free(req); EVP_PKEY_free(priv);
    return 1;
}

/* =========================================================================
 * Encoding helpers exposed under crypto.hex.*, crypto.base64.*,
 * crypto.base64url.*.
 * ======================================================================= */

static int crypto_hex_encode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    push_encoded(vm, (const unsigned char *)s->data, s->length, CRYPTO_ENC_HEX);
    return 1;
}

static int crypto_hex_decode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    if (s->length % 2 != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    unsigned char *out = (unsigned char *)cando_alloc(s->length / 2 + 1);
    long n = hex_decode(s->data, s->length, out);
    if (n < 0) {
        cando_free(out); cando_vm_push(vm, cando_null()); return 1;
    }
    libutil_push_str(vm, (const char *)out, (u32)n);
    cando_free(out);
    return 1;
}

static int crypto_base64_encode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    push_encoded(vm, (const unsigned char *)s->data, s->length, CRYPTO_ENC_BASE64);
    return 1;
}

static int crypto_base64_decode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    size_t out_len; bool urlsafe = false;
    unsigned char *out = b64_decode_dup(s->data, s->length, urlsafe, &out_len);
    if (!out) { cando_vm_push(vm, cando_null()); return 1; }
    libutil_push_str(vm, (const char *)out, (u32)out_len);
    cando_free(out);
    return 1;
}

static int crypto_base64url_encode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    push_encoded(vm, (const unsigned char *)s->data, s->length, CRYPTO_ENC_BASE64URL);
    return 1;
}

static int crypto_base64url_decode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) { libutil_push_str(vm, "", 0); return 1; }
    size_t out_len;
    unsigned char *out = b64_decode_dup(s->data, s->length, true, &out_len);
    if (!out) { cando_vm_push(vm, cando_null()); return 1; }
    libutil_push_str(vm, (const char *)out, (u32)out_len);
    cando_free(out);
    return 1;
}

/* Back-compat shims for the previous crypto.base64Encode /
 * crypto.base64Decode names. */
static int crypto_legacy_base64Encode(CandoVM *vm, int argc, CandoValue *args)
{ return crypto_base64_encode(vm, argc, args); }
static int crypto_legacy_base64Decode(CandoVM *vm, int argc, CandoValue *args)
{ return crypto_base64_decode(vm, argc, args); }

/* =========================================================================
 * timingSafeEqual
 * ======================================================================= */

static int crypto_timing_safe_equal(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2 || !cando_is_string(args[0]) || !cando_is_string(args[1])) {
        cando_vm_error(vm, "crypto.timingSafeEqual: expected (a, b)");
        return -1;
    }
    CandoString *a = cando_as_string(args[0]);
    CandoString *b = cando_as_string(args[1]);
    if (a->length != b->length) {
        cando_vm_push(vm, cando_bool(false));
        return 1;
    }
    int rc = CRYPTO_memcmp(a->data, b->data, a->length);
    cando_vm_push(vm, cando_bool(rc == 0));
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry crypto_top_methods[] = {
    /* Hashing */
    { "md5",          crypto_md5         },
    { "sha1",         crypto_sha1        },
    { "sha224",       crypto_sha224      },
    { "sha256",       crypto_sha256      },
    { "sha384",       crypto_sha384      },
    { "sha512",       crypto_sha512      },
    { "sha3_224",     crypto_sha3_224    },
    { "sha3_256",     crypto_sha3_256    },
    { "sha3_384",     crypto_sha3_384    },
    { "sha3_512",     crypto_sha3_512    },
    { "blake2b",      crypto_blake2b     },
    { "blake2s",      crypto_blake2s     },
    { "hash",         crypto_hash        },

    /* HMAC */
    { "hmac",         crypto_hmac        },

    /* KDF */
    { "pbkdf2",       crypto_pbkdf2      },
    { "scrypt",       crypto_scrypt      },
    { "hkdf",         crypto_hkdf        },

    /* Random */
    { "randomBytes",  crypto_random_bytes },
    { "randomInt",    crypto_random_int   },
    { "randomUUID",   crypto_random_uuid  },

    /* Symmetric */
    { "encrypt",      crypto_encrypt     },
    { "decrypt",      crypto_decrypt     },

    /* Asymmetric */
    { "generateKeyPair", crypto_generate_keypair },
    { "sign",            crypto_sign             },
    { "verify",          crypto_verify           },
    { "publicEncrypt",   crypto_public_encrypt   },
    { "privateDecrypt",  crypto_private_decrypt  },
    { "diffieHellman",   crypto_diffie_hellman   },

    /* Constant-time compare */
    { "timingSafeEqual", crypto_timing_safe_equal },

    /* Back-compat names */
    { "base64Encode", crypto_legacy_base64Encode },
    { "base64Decode", crypto_legacy_base64Decode },
};

static const LibutilMethodEntry crypto_hex_methods[] = {
    { "encode", crypto_hex_encode },
    { "decode", crypto_hex_decode },
};

static const LibutilMethodEntry crypto_base64_methods[] = {
    { "encode", crypto_base64_encode },
    { "decode", crypto_base64_decode },
};

static const LibutilMethodEntry crypto_base64url_methods[] = {
    { "encode", crypto_base64url_encode },
    { "decode", crypto_base64url_decode },
};

static const LibutilMethodEntry crypto_x509_methods[] = {
    { "create",      crypto_x509_create      },
    { "parse",       crypto_x509_parse       },
    { "verify",      crypto_x509_verify      },
    { "fingerprint", crypto_x509_fingerprint },
    { "csr",         crypto_x509_csr         },
};

static void attach_subobject(CandoVM *vm, CdoObject *parent, const char *key,
                              const LibutilMethodEntry *methods, usize count)
{
    CandoValue sub_val = cando_bridge_new_object(vm);
    CdoObject *sub_obj = cando_bridge_resolve(vm, cando_as_handle(sub_val));
    libutil_register_methods(vm, sub_obj, methods, count);

    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(parent, k, cando_bridge_to_cdo(vm, sub_val), FIELD_NONE);
    cdo_string_release(k);
    cando_value_release(sub_val);
}

void cando_lib_crypto_register(CandoVM *vm)
{
    CandoValue crypto_val = cando_bridge_new_object(vm);
    CdoObject *crypto_obj = cando_bridge_resolve(vm, cando_as_handle(crypto_val));

    libutil_register_methods(vm, crypto_obj, crypto_top_methods,
                             CANDO_ARRAY_LEN(crypto_top_methods));

    attach_subobject(vm, crypto_obj, "hex",       crypto_hex_methods,
                     CANDO_ARRAY_LEN(crypto_hex_methods));
    attach_subobject(vm, crypto_obj, "base64",    crypto_base64_methods,
                     CANDO_ARRAY_LEN(crypto_base64_methods));
    attach_subobject(vm, crypto_obj, "base64url", crypto_base64url_methods,
                     CANDO_ARRAY_LEN(crypto_base64url_methods));
    attach_subobject(vm, crypto_obj, "x509",      crypto_x509_methods,
                     CANDO_ARRAY_LEN(crypto_x509_methods));

    cando_vm_set_global(vm, "crypto", crypto_val, true);
}
