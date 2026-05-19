/*
 * tests/test_crypto.c -- C-level unit tests for the crypto library's
 * OpenSSL plumbing.
 *
 * These tests bypass the VM and exercise OpenSSL directly through the
 * same EVP / HMAC / KDF / cipher / asymmetric APIs that
 * source/lib/crypto.c uses.  They verify that:
 *
 *   - The OpenSSL build linked into libcando supports every algorithm
 *     the script-facing crypto.* API claims.
 *   - Known-Answer-Test vectors match (so any future refactor of the
 *     wrappers can be regression-checked at the C layer too).
 *   - The high-level pieces (Ed25519 sign/verify, AES-GCM AEAD,
 *     X.509 self-sign round-trip) compose correctly.
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

/* ---- minimal harness, same shape as tests/test_sockutil.c ----------- */
static int g_run = 0, g_passed = 0, g_failed = 0;
#define EXPECT(cond) do { \
    g_run++; \
    if (cond) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void hex_of(const unsigned char *b, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = d[(b[i] >> 4) & 0xF];
        out[i * 2 + 1] = d[ b[i]       & 0xF];
    }
    out[n * 2] = '\0';
}

/* ---- hash KAT vectors (RFC 6234 + NIST) ----------------------------- */

static void test_hash_vectors(void)
{
    struct { const char *algo; const char *input; const char *expected_hex; } vec[] = {
        /* SHA-256 of empty string. */
        { "sha256", "",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        /* SHA-256 of "abc". */
        { "sha256", "abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        /* SHA-512 of empty string. */
        { "sha512", "",
          "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
          "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
        /* SHA-1 of empty. */
        { "sha1", "", "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        /* SHA-3-256 of "abc" (NIST). */
        { "sha3-256", "abc",
          "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532" },
        /* SHA-3-512 of empty. */
        { "sha3-512", "",
          "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6"
          "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26" },
        { NULL, NULL, NULL }
    };
    for (int i = 0; vec[i].algo; i++) {
        const EVP_MD *md = EVP_get_digestbyname(vec[i].algo);
        EXPECT(md != NULL);
        if (!md) continue;
        unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dl;
        EVP_MD_CTX *c = EVP_MD_CTX_new();
        EVP_DigestInit_ex(c, md, NULL);
        EVP_DigestUpdate(c, vec[i].input, strlen(vec[i].input));
        EVP_DigestFinal_ex(c, dig, &dl);
        EVP_MD_CTX_free(c);
        char hex[EVP_MAX_MD_SIZE * 2 + 1];
        hex_of(dig, dl, hex);
        if (strcmp(hex, vec[i].expected_hex) != 0) {
            fprintf(stderr, "  KAT mismatch for %s(\"%s\"):\n    got      %s\n    expected %s\n",
                vec[i].algo, vec[i].input, hex, vec[i].expected_hex);
        }
        EXPECT(strcmp(hex, vec[i].expected_hex) == 0);
    }
}

/* ---- HMAC KAT (RFC 4231) -------------------------------------------- */

static void test_hmac_vector(void)
{
    /* HMAC-SHA-256("key", "The quick brown fox jumps over the lazy dog") */
    const char *key  = "key";
    const char *data = "The quick brown fox jumps over the lazy dog";
    const char *expected =
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";
    unsigned char out[EVP_MAX_MD_SIZE]; unsigned int ol;
    HMAC(EVP_sha256(), key, (int)strlen(key),
         (const unsigned char *)data, strlen(data),
         out, &ol);
    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_of(out, ol, hex);
    EXPECT(strcmp(hex, expected) == 0);
}

/* ---- PBKDF2 KAT (RFC 6070 vector 1) --------------------------------- */

static void test_pbkdf2_vector(void)
{
    const char *pw = "password";
    const char *salt = "salt";
    unsigned char out[20];
    int rc = PKCS5_PBKDF2_HMAC(pw, (int)strlen(pw),
                               (const unsigned char *)salt, (int)strlen(salt),
                               1, EVP_sha1(), 20, out);
    EXPECT(rc == 1);
    char hex[41];
    hex_of(out, 20, hex);
    EXPECT(strcmp(hex, "0c60c80f961f0e71f3a9b524af6012062fe037a6") == 0);
}

/* ---- HKDF KAT (RFC 5869 Test Case 1) -------------------------------- */

static void test_hkdf_vector(void)
{
    /* RFC 5869 Test Case 1: IKM is 22 bytes of 0x0b. */
    unsigned char ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));
    unsigned char salt[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c
    };
    unsigned char info[] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9
    };

    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EXPECT(kdf != NULL);
    EVP_KDF_CTX *kc = EVP_KDF_CTX_new(kdf);
    EXPECT(kc != NULL);
    OSSL_PARAM p[5];
    p[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, "SHA256", 0);
    p[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,  ikm,  sizeof(ikm));
    p[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, sizeof(salt));
    p[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, sizeof(info));
    p[4] = OSSL_PARAM_construct_end();
    unsigned char out[42];
    int rc = EVP_KDF_derive(kc, out, sizeof(out), p);
    EVP_KDF_CTX_free(kc);
    EVP_KDF_free(kdf);
    EXPECT(rc > 0);
    char hex[85];
    hex_of(out, 42, hex);
    /* Expected from RFC 5869 §A.1. */
    EXPECT(strcmp(hex,
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865") == 0);
}

/* ---- AES-256-GCM round-trip ---------------------------------------- */

static void test_aes_256_gcm_roundtrip(void)
{
    unsigned char key[32], iv[12];
    RAND_bytes(key, sizeof(key));
    RAND_bytes(iv,  sizeof(iv));
    const char *pt = "the secret message";
    size_t plen = strlen(pt);
    unsigned char ct[64], tag[16];
    int outl = 0, ct_len = 0;

    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    EXPECT(c != NULL);
    EXPECT(EVP_EncryptInit_ex2(c, EVP_aes_256_gcm(), key, iv, NULL) == 1);
    EXPECT(EVP_EncryptUpdate(c, ct, &outl, (const unsigned char *)pt, (int)plen) == 1);
    ct_len = outl;
    EXPECT(EVP_EncryptFinal_ex(c, ct + ct_len, &outl) == 1);
    ct_len += outl;
    EXPECT(EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
    EVP_CIPHER_CTX_free(c);

    unsigned char back[64];
    c = EVP_CIPHER_CTX_new();
    EXPECT(EVP_DecryptInit_ex2(c, EVP_aes_256_gcm(), key, iv, NULL) == 1);
    EXPECT(EVP_DecryptUpdate(c, back, &outl, ct, ct_len) == 1);
    int back_len = outl;
    EXPECT(EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1);
    EXPECT(EVP_DecryptFinal_ex(c, back + back_len, &outl) == 1);
    back_len += outl;
    EVP_CIPHER_CTX_free(c);

    EXPECT((size_t)back_len == plen);
    EXPECT(memcmp(back, pt, plen) == 0);
}

/* ---- Ed25519 sign/verify ------------------------------------------- */

static void test_ed25519(void)
{
    EVP_PKEY *pk = NULL;
    EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_from_name(NULL, "ED25519", NULL);
    EXPECT(kc != NULL);
    EXPECT(EVP_PKEY_keygen_init(kc) > 0);
    EXPECT(EVP_PKEY_keygen(kc, &pk) > 0);
    EVP_PKEY_CTX_free(kc);
    EXPECT(pk != NULL);

    const char *msg = "hello, ed25519";
    EVP_MD_CTX *mc = EVP_MD_CTX_new();
    EXPECT(EVP_DigestSignInit(mc, NULL, NULL, NULL, pk) == 1);
    size_t sig_len = 0;
    EXPECT(EVP_DigestSign(mc, NULL, &sig_len,
                          (const unsigned char *)msg, strlen(msg)) == 1);
    unsigned char *sig = malloc(sig_len);
    EXPECT(EVP_DigestSign(mc, sig, &sig_len,
                          (const unsigned char *)msg, strlen(msg)) == 1);
    EVP_MD_CTX_free(mc);
    EXPECT(sig_len == 64);

    mc = EVP_MD_CTX_new();
    EXPECT(EVP_DigestVerifyInit(mc, NULL, NULL, NULL, pk) == 1);
    EXPECT(EVP_DigestVerify(mc, sig, sig_len,
                            (const unsigned char *)msg, strlen(msg)) == 1);
    /* Wrong message → 0. */
    EXPECT(EVP_DigestVerify(mc, sig, sig_len,
                            (const unsigned char *)"oops", 4) == 0);
    EVP_MD_CTX_free(mc);

    free(sig);
    EVP_PKEY_free(pk);
}

/* ---- X.509 self-signed round-trip ---------------------------------- */

static void test_x509_self_signed(void)
{
    /* Generate an RSA 2048 key, build a self-signed cert, parse it back. */
    EVP_PKEY *pk = NULL;
    EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    EXPECT(kc != NULL);
    EVP_PKEY_keygen_init(kc);
    EVP_PKEY_CTX_set_rsa_keygen_bits(kc, 2048);
    EXPECT(EVP_PKEY_keygen(kc, &pk) > 0);
    EVP_PKEY_CTX_free(kc);

    X509 *crt = X509_new();
    EXPECT(crt != NULL);
    X509_set_version(crt, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
    X509_gmtime_adj(X509_get_notBefore(crt), 0);
    X509_gmtime_adj(X509_get_notAfter(crt), 60 * 60 * 24 * 365);
    X509_set_pubkey(crt, pk);
    X509_NAME *nm = X509_NAME_new();
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_UTF8,
        (const unsigned char *)"test.example", -1, -1, 0);
    X509_set_subject_name(crt, nm);
    X509_set_issuer_name(crt, nm);
    EXPECT(X509_sign(crt, pk, EVP_sha256()) != 0);
    X509_NAME_free(nm);

    /* Serialize + parse back. */
    BIO *b = BIO_new(BIO_s_mem());
    EXPECT(PEM_write_bio_X509(b, crt) == 1);
    BUF_MEM *bm; BIO_get_mem_ptr(b, &bm);
    BIO *rb = BIO_new_mem_buf(bm->data, (int)bm->length);
    X509 *parsed = PEM_read_bio_X509(rb, NULL, NULL, NULL);
    BIO_free(rb);
    EXPECT(parsed != NULL);

    /* Subject CN should round-trip. */
    char buf[64];
    X509_NAME_get_text_by_NID(X509_get_subject_name(parsed),
                              NID_commonName, buf, sizeof(buf));
    EXPECT(strcmp(buf, "test.example") == 0);

    X509_free(parsed);
    X509_free(crt);
    BIO_free(b);
    EVP_PKEY_free(pk);
}

/* ---- timingSafeEqual ----------------------------------------------- */

static void test_timing_safe_equal(void)
{
    /* CRYPTO_memcmp is constant-time. */
    EXPECT(CRYPTO_memcmp("abc", "abc", 3) == 0);
    EXPECT(CRYPTO_memcmp("abc", "abd", 3) != 0);
}

int main(void)
{
    test_hash_vectors();
    test_hmac_vector();
    test_pbkdf2_vector();
    test_hkdf_vector();
    test_aes_256_gcm_roundtrip();
    test_ed25519();
    test_x509_self_signed();
    test_timing_safe_equal();

    printf("test_crypto: %d run, %d passed, %d failed\n",
           g_run, g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
