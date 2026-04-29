/*
 * modules/sql/sql_crypto.h -- Hashing / HMAC / PBKDF2 / random / base64
 * helpers backed by OpenSSL.
 *
 * libcando already links OpenSSL (the `socket`, `secure_socket`, `http`,
 * and `https` libraries depend on it), so MD5 / SHA1 / SHA256 / HMAC /
 * PKCS5_PBKDF2_HMAC are all available without bringing a third-party
 * dependency on top of what cando itself ships with.  The helpers here
 * wrap the EVP API into one-shot calls so the drivers can compute a
 * digest in a single line.
 *
 * Header-only so each driver translation unit can include it without
 * adding a separate object file to the link line.
 */

#ifndef CANDO_SQL_CRYPTO_H
#define CANDO_SQL_CRYPTO_H

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Digest sizes -- avoid pulling in <openssl/md5.h>/<sha.h>, which are
 * deprecated headers in OpenSSL 3.x. */
#define SQL_MD5_LEN    16
#define SQL_SHA1_LEN   20
#define SQL_SHA256_LEN 32

/* sql_digest -- compute a one-shot digest with the named EVP_MD.
 * Returns true on success.  The output buffer must be large enough
 * (use SQL_*_LEN constants). */
static inline bool sql_digest(const EVP_MD *md,
                              const void *data, size_t n,
                              unsigned char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestInit_ex(ctx, md, NULL) == 1
           && EVP_DigestUpdate(ctx, data, n)   == 1
           && EVP_DigestFinal_ex(ctx, out, NULL) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

static inline bool sql_md5(const void *data, size_t n, unsigned char out[SQL_MD5_LEN])
{
    return sql_digest(EVP_md5(), data, n, out);
}

static inline bool sql_sha1(const void *data, size_t n, unsigned char out[SQL_SHA1_LEN])
{
    return sql_digest(EVP_sha1(), data, n, out);
}

static inline bool sql_sha256(const void *data, size_t n, unsigned char out[SQL_SHA256_LEN])
{
    return sql_digest(EVP_sha256(), data, n, out);
}

/* sql_hmac -- one-shot HMAC.  out_len receives the digest length. */
static inline bool sql_hmac(const EVP_MD *md,
                            const void *key, size_t key_len,
                            const void *data, size_t data_len,
                            unsigned char *out, unsigned int *out_len)
{
    return HMAC(md, key, (int)key_len,
                (const unsigned char *)data, data_len,
                out, out_len) != NULL;
}

static inline bool sql_hmac_sha256(const void *key, size_t key_len,
                                   const void *data, size_t data_len,
                                   unsigned char out[SQL_SHA256_LEN])
{
    unsigned int outl = 0;
    if (!sql_hmac(EVP_sha256(), key, key_len, data, data_len, out, &outl))
        return false;
    return outl == SQL_SHA256_LEN;
}

/* sql_pbkdf2_sha256 -- PBKDF2-HMAC-SHA-256 (used for SCRAM). */
static inline bool sql_pbkdf2_sha256(const char *pass, size_t pass_len,
                                     const unsigned char *salt, size_t salt_len,
                                     int iter, unsigned char out[SQL_SHA256_LEN])
{
    return PKCS5_PBKDF2_HMAC(pass, (int)pass_len,
                             salt, (int)salt_len, iter,
                             EVP_sha256(),
                             SQL_SHA256_LEN, out) == 1;
}

/* sql_random_bytes -- cryptographically strong random.  Used for SCRAM
 * client nonces. */
static inline bool sql_random_bytes(unsigned char *out, size_t n)
{
    return RAND_bytes(out, (int)n) == 1;
}

/* =========================================================================
 * Hex encoding -- used for PostgreSQL md5 password
 * ===================================================================== */

static inline void sql_hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[ in[i]       & 0xf];
    }
    out[n * 2] = '\0';
}

/* =========================================================================
 * Base64 encoding (no line wrapping, '=' padding) -- standalone so the
 * drivers don't need to thread cando's own base64 helper through the
 * crypto layer.
 * ===================================================================== */

static inline size_t sql_b64_encode_len(size_t n)
{
    return ((n + 2) / 3) * 4;
}

static inline void sql_b64_encode(const unsigned char *in, size_t n, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, j;
    for (i = 0, j = 0; i + 2 < n; i += 3) {
        uint32_t t = ((uint32_t)in[i] << 16)
                   | ((uint32_t)in[i+1] << 8)
                   |  (uint32_t)in[i+2];
        out[j++] = tab[(t >> 18) & 0x3f];
        out[j++] = tab[(t >> 12) & 0x3f];
        out[j++] = tab[(t >>  6) & 0x3f];
        out[j++] = tab[ t        & 0x3f];
    }
    if (i < n) {
        uint32_t t = (uint32_t)in[i] << 16;
        if (i + 1 < n) t |= (uint32_t)in[i+1] << 8;
        out[j++] = tab[(t >> 18) & 0x3f];
        out[j++] = tab[(t >> 12) & 0x3f];
        out[j++] = (i + 1 < n) ? tab[(t >> 6) & 0x3f] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* sql_b64_decode -- returns number of decoded bytes; -1 on malformed
 * input.  out must be at least (in_len/4)*3 bytes. */
static inline int sql_b64_decode(const char *in, size_t in_len,
                                 unsigned char *out)
{
    if (in_len % 4 != 0) return -1;
    size_t out_len = (in_len / 4) * 3;
    if (in_len > 0 && in[in_len - 1] == '=') out_len--;
    if (in_len > 1 && in[in_len - 2] == '=') out_len--;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            if      (c >= 'A' && c <= 'Z') v[k] = c - 'A';
            else if (c >= 'a' && c <= 'z') v[k] = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v[k] = c - '0' + 52;
            else if (c == '+')             v[k] = 62;
            else if (c == '/')             v[k] = 63;
            else if (c == '=')             v[k] = 0;
            else return -1;
        }
        uint32_t t = ((uint32_t)v[0] << 18)
                   | ((uint32_t)v[1] << 12)
                   | ((uint32_t)v[2] <<  6)
                   |  (uint32_t)v[3];
        if (j < out_len) out[j++] = (unsigned char)((t >> 16) & 0xff);
        if (j < out_len) out[j++] = (unsigned char)((t >>  8) & 0xff);
        if (j < out_len) out[j++] = (unsigned char)( t        & 0xff);
    }
    return (int)out_len;
}

#endif /* CANDO_SQL_CRYPTO_H */
