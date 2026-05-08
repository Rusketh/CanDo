/*
 * lib/crypto.c -- Cryptography and hashing standard library for Cando.
 *
 * Backed by OpenSSL (-lcrypto), which the rest of the runtime already
 * links for the secure socket and HTTPS modules.
 *
 * Must compile with gcc -std=c11.
 */

#include "crypto.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

/* =========================================================================
 * Base64 Encoding
 * ======================================================================= */

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_char_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int crypto_base64Encode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s) {
        libutil_push_str(vm, "", 0);
        return 1;
    }

    u32 len = s->length;
    u32 out_len = 4 * ((len + 2) / 3);
    char *out = (char *)cando_alloc(out_len + 1);

    for (u32 i = 0, j = 0; i < len;) {
        u32 group_start = i;
        u32 octet_a = (u32)(u8)s->data[i++];
        u32 octet_b = (i < len) ? (u32)(u8)s->data[i++] : 0;
        u32 octet_c = (i < len) ? (u32)(u8)s->data[i++] : 0;
        u32 bytes_in_group = i - group_start;

        u32 triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        out[j++] = base64_table[(triple >> 3 * 6) & 0x3F];
        out[j++] = base64_table[(triple >> 2 * 6) & 0x3F];
        out[j++] = (bytes_in_group < 2) ? '=' : base64_table[(triple >> 1 * 6) & 0x3F];
        out[j++] = (bytes_in_group < 3) ? '=' : base64_table[(triple >> 0 * 6) & 0x3F];
    }
    out[out_len] = '\0';

    libutil_push_str(vm, out, out_len);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * MD5 and SHA256, backed by OpenSSL EVP
 * ======================================================================= */

static int crypto_hash_evp(CandoVM *vm, int argc, CandoValue *args,
                            const EVP_MD *md, const char *name)
{
    /* Default to the empty-string digest if the caller passes anything
     * non-stringy -- matches the previous placeholder's "always succeed"
     * shape so existing scripts keep working. */
    const char *data = "";
    u32         len  = 0;
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (s) { data = s->data; len = s->length; }

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
        cando_vm_error(vm, "crypto.%s: digest computation failed", name);
        return -1;
    }
    EVP_MD_CTX_free(ctx);

    /* Hex-encode into a stack buffer.  EVP_MAX_MD_SIZE is 64, so the
     * encoded form is at most 128 bytes plus the trailing NUL. */
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < digest_len; i++) {
        hex[i * 2]     = digits[(digest[i] >> 4) & 0xF];
        hex[i * 2 + 1] = digits[ digest[i]       & 0xF];
    }
    hex[digest_len * 2] = '\0';

    libutil_push_str(vm, hex, digest_len * 2);
    return 1;
}

static int crypto_md5(CandoVM *vm, int argc, CandoValue *args)
{
    return crypto_hash_evp(vm, argc, args, EVP_md5(), "md5");
}

static int crypto_sha256(CandoVM *vm, int argc, CandoValue *args)
{
    return crypto_hash_evp(vm, argc, args, EVP_sha256(), "sha256");
}

static int crypto_base64Decode(CandoVM *vm, int argc, CandoValue *args)
{
    CandoString *s = libutil_arg_str_at(args, argc, 0);
    if (!s || s->length % 4 != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }

    u32 len = s->length;
    u32 out_len = (len / 4) * 3;
    if (len > 0 && s->data[len - 1] == '=') out_len--;
    if (len > 1 && s->data[len - 2] == '=') out_len--;

    char *out = (char *)cando_alloc(out_len + 1);
    for (u32 i = 0, j = 0; i < len;) {
        int a = base64_char_value(s->data[i++]);
        int b = base64_char_value(s->data[i++]);
        int c = (s->data[i] == '=') ? 0 : base64_char_value(s->data[i]); i++;
        int d = (s->data[i] == '=') ? 0 : base64_char_value(s->data[i]); i++;

        if (a < 0 || b < 0 || c < 0 || d < 0) {
            cando_free(out);
            cando_vm_push(vm, cando_null());
            return 1;
        }

        u32 triple = (u32)((a << 18) + (b << 12) + (c << 6) + d);
        if (j < out_len) out[j++] = (char)((triple >> 16) & 0xFF);
        if (j < out_len) out[j++] = (char)((triple >> 8) & 0xFF);
        if (j < out_len) out[j++] = (char)(triple & 0xFF);
    }
    out[out_len] = '\0';
    libutil_push_str(vm, out, out_len);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

static const LibutilMethodEntry crypto_methods[] = {
    { "md5",          crypto_md5          },
    { "sha256",       crypto_sha256       },
    { "base64Encode", crypto_base64Encode },
    { "base64Decode", crypto_base64Decode },
};

void cando_lib_crypto_register(CandoVM *vm)
{
    CandoValue crypto_val = cando_bridge_new_object(vm);
    CdoObject *crypto_obj = cando_bridge_resolve(vm, cando_as_handle(crypto_val));

    libutil_register_methods(vm, crypto_obj, crypto_methods,
                             CANDO_ARRAY_LEN(crypto_methods));

    cando_vm_set_global(vm, "crypto", crypto_val, true);
}
