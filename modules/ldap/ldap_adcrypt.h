/*
 * modules/ldap/ldap_adcrypt.h -- AD reversible-encryption decoder
 *
 * Self-contained MD5 + RC4 implementations and the high-level
 * `decode_reversible_password` used by the LDAP module.
 *
 * Active Directory stores passwords with reversible encryption (when the
 * group policy "Store password using reversible encryption" is enabled)
 * inside the user's `supplementalCredentials` attribute, in the
 * "Primary:CLEARTEXT" property.  That property is RC4-encrypted with a
 * key derived from the syskey, the user's RID, and a per-record salt.
 *
 * Retrieving the encrypted blob requires Domain Admin / DRS replication
 * permissions (DCSync) -- a regular LDAP bind cannot read it.  Once the
 * caller has the bytes, this header decodes them.
 *
 * Inputs to decode_reversible_password:
 *   - blob    : the RC4-encrypted ciphertext bytes
 *   - key     : the already-derived 16-byte RC4 key (MD5(syskey || rid_le ||
 *               b"!@#$%^&*()qwertyUIOPAzxcvbnmQQQQQQQQQQQQ)(*@&%") for
 *               typical AD setups; callers compose this themselves to keep
 *               the module agnostic of registry layouts.)
 *
 * The decrypted plaintext is UTF-16LE; the helper converts to UTF-8 for
 * the caller.
 */

#ifndef CANDO_LDAP_ADCRYPT_H
#define CANDO_LDAP_ADCRYPT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------- MD5 (RFC 1321) ---------- */

typedef struct {
    uint32_t state[4];
    uint64_t count;       /* bits */
    uint8_t  buffer[64];
} ADC_MD5_CTX;

static inline uint32_t adc_md5_F(uint32_t x, uint32_t y, uint32_t z)
    { return (x & y) | (~x & z); }
static inline uint32_t adc_md5_G(uint32_t x, uint32_t y, uint32_t z)
    { return (x & z) | (y & ~z); }
static inline uint32_t adc_md5_H(uint32_t x, uint32_t y, uint32_t z)
    { return x ^ y ^ z; }
static inline uint32_t adc_md5_I(uint32_t x, uint32_t y, uint32_t z)
    { return y ^ (x | ~z); }
static inline uint32_t adc_md5_rot(uint32_t v, int s)
    { return (v << s) | (v >> (32 - s)); }

static inline void adc_md5_compress(ADC_MD5_CTX *ctx, const uint8_t blk[64])
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)blk[i*4] | ((uint32_t)blk[i*4+1] << 8)
             | ((uint32_t)blk[i*4+2] << 16) | ((uint32_t)blk[i*4+3] << 24);
    }
    uint32_t a = ctx->state[0], b = ctx->state[1],
             c = ctx->state[2], d = ctx->state[3];

#define ADC_FF(a,b,c,d,x,s,ac) \
    a = b + adc_md5_rot(a + adc_md5_F(b,c,d) + (x) + (ac), (s))
#define ADC_GG(a,b,c,d,x,s,ac) \
    a = b + adc_md5_rot(a + adc_md5_G(b,c,d) + (x) + (ac), (s))
#define ADC_HH(a,b,c,d,x,s,ac) \
    a = b + adc_md5_rot(a + adc_md5_H(b,c,d) + (x) + (ac), (s))
#define ADC_II(a,b,c,d,x,s,ac) \
    a = b + adc_md5_rot(a + adc_md5_I(b,c,d) + (x) + (ac), (s))

    ADC_FF(a,b,c,d,w[ 0], 7,0xd76aa478); ADC_FF(d,a,b,c,w[ 1],12,0xe8c7b756);
    ADC_FF(c,d,a,b,w[ 2],17,0x242070db); ADC_FF(b,c,d,a,w[ 3],22,0xc1bdceee);
    ADC_FF(a,b,c,d,w[ 4], 7,0xf57c0faf); ADC_FF(d,a,b,c,w[ 5],12,0x4787c62a);
    ADC_FF(c,d,a,b,w[ 6],17,0xa8304613); ADC_FF(b,c,d,a,w[ 7],22,0xfd469501);
    ADC_FF(a,b,c,d,w[ 8], 7,0x698098d8); ADC_FF(d,a,b,c,w[ 9],12,0x8b44f7af);
    ADC_FF(c,d,a,b,w[10],17,0xffff5bb1); ADC_FF(b,c,d,a,w[11],22,0x895cd7be);
    ADC_FF(a,b,c,d,w[12], 7,0x6b901122); ADC_FF(d,a,b,c,w[13],12,0xfd987193);
    ADC_FF(c,d,a,b,w[14],17,0xa679438e); ADC_FF(b,c,d,a,w[15],22,0x49b40821);

    ADC_GG(a,b,c,d,w[ 1], 5,0xf61e2562); ADC_GG(d,a,b,c,w[ 6], 9,0xc040b340);
    ADC_GG(c,d,a,b,w[11],14,0x265e5a51); ADC_GG(b,c,d,a,w[ 0],20,0xe9b6c7aa);
    ADC_GG(a,b,c,d,w[ 5], 5,0xd62f105d); ADC_GG(d,a,b,c,w[10], 9,0x02441453);
    ADC_GG(c,d,a,b,w[15],14,0xd8a1e681); ADC_GG(b,c,d,a,w[ 4],20,0xe7d3fbc8);
    ADC_GG(a,b,c,d,w[ 9], 5,0x21e1cde6); ADC_GG(d,a,b,c,w[14], 9,0xc33707d6);
    ADC_GG(c,d,a,b,w[ 3],14,0xf4d50d87); ADC_GG(b,c,d,a,w[ 8],20,0x455a14ed);
    ADC_GG(a,b,c,d,w[13], 5,0xa9e3e905); ADC_GG(d,a,b,c,w[ 2], 9,0xfcefa3f8);
    ADC_GG(c,d,a,b,w[ 7],14,0x676f02d9); ADC_GG(b,c,d,a,w[12],20,0x8d2a4c8a);

    ADC_HH(a,b,c,d,w[ 5], 4,0xfffa3942); ADC_HH(d,a,b,c,w[ 8],11,0x8771f681);
    ADC_HH(c,d,a,b,w[11],16,0x6d9d6122); ADC_HH(b,c,d,a,w[14],23,0xfde5380c);
    ADC_HH(a,b,c,d,w[ 1], 4,0xa4beea44); ADC_HH(d,a,b,c,w[ 4],11,0x4bdecfa9);
    ADC_HH(c,d,a,b,w[ 7],16,0xf6bb4b60); ADC_HH(b,c,d,a,w[10],23,0xbebfbc70);
    ADC_HH(a,b,c,d,w[13], 4,0x289b7ec6); ADC_HH(d,a,b,c,w[ 0],11,0xeaa127fa);
    ADC_HH(c,d,a,b,w[ 3],16,0xd4ef3085); ADC_HH(b,c,d,a,w[ 6],23,0x04881d05);
    ADC_HH(a,b,c,d,w[ 9], 4,0xd9d4d039); ADC_HH(d,a,b,c,w[12],11,0xe6db99e5);
    ADC_HH(c,d,a,b,w[15],16,0x1fa27cf8); ADC_HH(b,c,d,a,w[ 2],23,0xc4ac5665);

    ADC_II(a,b,c,d,w[ 0], 6,0xf4292244); ADC_II(d,a,b,c,w[ 7],10,0x432aff97);
    ADC_II(c,d,a,b,w[14],15,0xab9423a7); ADC_II(b,c,d,a,w[ 5],21,0xfc93a039);
    ADC_II(a,b,c,d,w[12], 6,0x655b59c3); ADC_II(d,a,b,c,w[ 3],10,0x8f0ccc92);
    ADC_II(c,d,a,b,w[10],15,0xffeff47d); ADC_II(b,c,d,a,w[ 1],21,0x85845dd1);
    ADC_II(a,b,c,d,w[ 8], 6,0x6fa87e4f); ADC_II(d,a,b,c,w[15],10,0xfe2ce6e0);
    ADC_II(c,d,a,b,w[ 6],15,0xa3014314); ADC_II(b,c,d,a,w[13],21,0x4e0811a1);
    ADC_II(a,b,c,d,w[ 4], 6,0xf7537e82); ADC_II(d,a,b,c,w[11],10,0xbd3af235);
    ADC_II(c,d,a,b,w[ 2],15,0x2ad7d2bb); ADC_II(b,c,d,a,w[ 9],21,0xeb86d391);

#undef ADC_FF
#undef ADC_GG
#undef ADC_HH
#undef ADC_II

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static inline void adc_md5_init(ADC_MD5_CTX *ctx)
{
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

static inline void adc_md5_update(ADC_MD5_CTX *ctx,
                                  const uint8_t *data, size_t len)
{
    size_t off = (size_t)((ctx->count >> 3) & 63);
    ctx->count += (uint64_t)len << 3;
    size_t take = 64 - off;
    if (len >= take) {
        if (off) {
            memcpy(ctx->buffer + off, data, take);
            adc_md5_compress(ctx, ctx->buffer);
            data += take; len -= take; off = 0;
        }
        while (len >= 64) {
            adc_md5_compress(ctx, data);
            data += 64; len -= 64;
        }
    }
    if (len) memcpy(ctx->buffer + off, data, len);
}

static inline void adc_md5_final(ADC_MD5_CTX *ctx, uint8_t out[16])
{
    static const uint8_t pad[64] = { 0x80 };
    uint64_t total_bits = ctx->count;
    uint8_t bits_le[8];
    for (int i = 0; i < 8; i++) bits_le[i] = (uint8_t)(total_bits >> (i*8));
    size_t off = (size_t)((ctx->count >> 3) & 63);
    size_t pad_len = (off < 56) ? (56 - off) : (120 - off);
    adc_md5_update(ctx, pad, pad_len);
    adc_md5_update(ctx, bits_le, 8);
    for (int i = 0; i < 4; i++) {
        out[i*4]   = (uint8_t)(ctx->state[i]);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

static inline void adc_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    ADC_MD5_CTX ctx; adc_md5_init(&ctx);
    adc_md5_update(&ctx, data, len);
    adc_md5_final(&ctx, out);
}

/* ---------- RC4 (ARCFOUR) ---------- */

typedef struct { uint8_t s[256]; int i, j; } ADC_RC4_CTX;

static inline void adc_rc4_init(ADC_RC4_CTX *ctx,
                                const uint8_t *key, size_t key_len)
{
    for (int i = 0; i < 256; i++) ctx->s[i] = (uint8_t)i;
    int j = 0;
    if (key_len > 0) {
        for (int i = 0; i < 256; i++) {
            j = (j + ctx->s[i] + key[i % key_len]) & 0xFF;
            uint8_t t = ctx->s[i]; ctx->s[i] = ctx->s[j]; ctx->s[j] = t;
        }
    }
    ctx->i = 0; ctx->j = 0;
}

static inline void adc_rc4_xor(ADC_RC4_CTX *ctx,
                               const uint8_t *in, uint8_t *out, size_t n)
{
    int i = ctx->i, j = ctx->j;
    for (size_t k = 0; k < n; k++) {
        i = (i + 1) & 0xFF;
        j = (j + ctx->s[i]) & 0xFF;
        uint8_t t = ctx->s[i]; ctx->s[i] = ctx->s[j]; ctx->s[j] = t;
        uint8_t kbyte = ctx->s[(ctx->s[i] + ctx->s[j]) & 0xFF];
        out[k] = in[k] ^ kbyte;
    }
    ctx->i = i; ctx->j = j;
}

/* ---------- High-level helpers ---------- */

/*
 * adc_decode_reversible -- RC4-decrypt `blob` with `key` and write the
 * resulting bytes (interpreted as raw bytes; the caller may treat them as
 * UTF-16LE) to `out`.
 *
 * `out` must be at least `blob_len` bytes.  Returns the number of bytes
 * written (== blob_len).  Constant-time over the input length.
 */
static inline size_t adc_decode_reversible(const uint8_t *blob, size_t blob_len,
                                           const uint8_t *key, size_t key_len,
                                           uint8_t *out)
{
    ADC_RC4_CTX rc4;
    adc_rc4_init(&rc4, key, key_len);
    adc_rc4_xor(&rc4, blob, out, blob_len);
    return blob_len;
}

/*
 * adc_utf16le_to_utf8 -- convert a UTF-16LE byte buffer (in_len bytes,
 * must be even) to UTF-8 in `out` (size `out_cap`).  Returns the number
 * of UTF-8 bytes written, or -1 on buffer-too-small / malformed input.
 */
static inline long adc_utf16le_to_utf8(const uint8_t *in, size_t in_len,
                                       char *out, size_t out_cap)
{
    if (in_len & 1) return -1;
    size_t o = 0;
    size_t i = 0;
    while (i < in_len) {
        uint32_t cp = (uint32_t)in[i] | ((uint32_t)in[i+1] << 8);
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 2 > in_len) return -1;
            uint32_t lo = (uint32_t)in[i] | ((uint32_t)in[i+1] << 8);
            i += 2;
            if (lo < 0xDC00 || lo > 0xDFFF) return -1;
            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
        }
        if (cp < 0x80) {
            if (o + 1 > out_cap) return -1;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 > out_cap) return -1;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 > out_cap) return -1;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 > out_cap) return -1;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return (long)o;
}

#endif /* CANDO_LDAP_ADCRYPT_H */
