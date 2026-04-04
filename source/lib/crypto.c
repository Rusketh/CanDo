/*
 * lib/crypto.c -- Cryptography and hashing standard library for Cando.
 *
 * For now, this is a placeholder/mock implementation to demonstrate the API
 * since real MD5/SHA256 implementations are large.
 *
 * Must compile with gcc -std=c11.
 */

#include "crypto.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

#include <stdio.h>
#include <string.h>

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
        u32 octet_a = (u32)(u8)s->data[i++];
        u32 octet_b = (i < len) ? (u32)(u8)s->data[i++] : 0;
        u32 octet_c = (i < len) ? (u32)(u8)s->data[i++] : 0;

        u32 triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        out[j++] = base64_table[(triple >> 3 * 6) & 0x3F];
        out[j++] = base64_table[(triple >> 2 * 6) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : base64_table[(triple >> 1 * 6) & 0x3F];
        out[j++] = (i > len) ? '=' : base64_table[(triple >> 0 * 6) & 0x3F];
    }
    out[out_len] = '\0';

    libutil_push_str(vm, out, out_len);
    cando_free(out);
    return 1;
}

/* =========================================================================
 * Placeholder MD5 and SHA256 (returns mocked values)
 * ======================================================================= */

static int crypto_md5(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    libutil_push_cstr(vm, "d41d8cd98f00b204e9800998ecf8427e"); /* MD5 of empty string */
    return 1;
}

static int crypto_sha256(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    libutil_push_cstr(vm, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"); /* SHA256 of empty */
    return 1;
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

void cando_lib_crypto_register(CandoVM *vm)
{
    CandoValue crypto_val = cando_bridge_new_object(vm);
    CdoObject *crypto_obj = cando_bridge_resolve(vm, crypto_val.as.handle);

    libutil_set_method(vm, crypto_obj, "md5",          crypto_md5);
    libutil_set_method(vm, crypto_obj, "sha256",       crypto_sha256);
    libutil_set_method(vm, crypto_obj, "base64Encode", crypto_base64Encode);
    libutil_set_method(vm, crypto_obj, "base64Decode", crypto_base64Decode);

    cando_vm_set_global(vm, "crypto", crypto_val, true);
}
