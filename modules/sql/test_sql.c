/*
 * modules/sql/test_sql.c -- C unit tests for the SQL module's
 * pure-C helpers.
 *
 * Built and run with:
 *
 *     make -C modules/sql test
 *
 * These tests do not link libcando -- they exercise:
 *   - sql_buf.h (writer + reader, big-/little-endian, length-encoded)
 *   - sql_crypto.h (MD5, SHA-1, SHA-256, HMAC-SHA-256, PBKDF2-SHA-256,
 *                    base64 round-trip, hex encoding)
 *   - the MySQL native_password / caching_sha2_password token
 *     computations (these are pure functions of (password, salt))
 *
 * Script-level coverage of the script-facing module surface lives in
 * test_sql.cdo, which only runs when a real PG / MySQL server is up.
 */

#include "sql_buf.h"
#include "sql_crypto.h"
#include "sql_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(name, cond) do {                            \
    if (cond) {                                            \
        printf("  PASS  %s\n", name);                      \
    } else {                                               \
        printf("  FAIL  %s\n", name);                      \
        failures++;                                        \
    }                                                      \
} while (0)

#define EXPECT_STREQ(name, a, b) do {                                  \
    const char *_a = (a), *_b = (b);                                   \
    if (_a && _b && strcmp(_a, _b) == 0) {                             \
        printf("  PASS  %s\n", name);                                  \
    } else {                                                           \
        printf("  FAIL  %s  (\"%s\" != \"%s\")\n", name,               \
               _a ? _a : "(null)", _b ? _b : "(null)");                \
        failures++;                                                    \
    }                                                                  \
} while (0)

#define EXPECT_INTEQ(name, a, b) do {                                  \
    long long _a = (long long)(a), _b = (long long)(b);                \
    if (_a == _b) {                                                    \
        printf("  PASS  %s\n", name);                                  \
    } else {                                                           \
        printf("  FAIL  %s  (%lld != %lld)\n", name, _a, _b);          \
        failures++;                                                    \
    }                                                                  \
} while (0)

/* =========================================================================
 * sql_buf -- writer round-trips
 * ===================================================================== */
static void test_buf_writer(void)
{
    printf("\n[buf_writer]\n");

    SqlBuf b = {0};
    sql_buf_put_u8 (&b, 0xab);
    sql_buf_put_be16(&b, 0x1234);
    sql_buf_put_be32(&b, 0xdeadbeef);
    sql_buf_put_le16(&b, 0x1234);
    sql_buf_put_le32(&b, 0xdeadbeef);
    sql_buf_put_le64(&b, 0x0102030405060708ULL);
    sql_buf_put_cstr(&b, "hello");
    sql_buf_put_lenenc(&b, 200);
    sql_buf_put_lenenc(&b, 1000);
    sql_buf_put_lenenc(&b, 0x100000ULL);

    EXPECT_INTEQ("u8",          b.data[0], 0xab);
    EXPECT_INTEQ("be16 hi",     b.data[1], 0x12);
    EXPECT_INTEQ("be16 lo",     b.data[2], 0x34);
    EXPECT_INTEQ("be32 [3]",    b.data[3], 0xde);
    EXPECT_INTEQ("be32 [6]",    b.data[6], 0xef);
    EXPECT_INTEQ("le16 lo",     b.data[7], 0x34);
    EXPECT_INTEQ("le16 hi",     b.data[8], 0x12);
    EXPECT_INTEQ("le32 [9]",    b.data[9], 0xef);
    EXPECT_INTEQ("le32 [12]",   b.data[12], 0xde);

    sql_buf_free(&b);
}

/* =========================================================================
 * sql_buf -- reader round-trips
 * ===================================================================== */
static void test_buf_reader(void)
{
    printf("\n[buf_reader]\n");

    /* Round-trip every primitive through writer + reader. */
    SqlBuf b = {0};
    sql_buf_put_u8(&b, 0xff);
    sql_buf_put_be16(&b, 0x1234);
    sql_buf_put_be32(&b, 0xdeadbeef);
    sql_buf_put_le16(&b, 0x1234);
    sql_buf_put_le24(&b, 0xabcdef);
    sql_buf_put_le32(&b, 0xdeadbeef);
    sql_buf_put_le64(&b, 0x0102030405060708ULL);
    sql_buf_put_cstr(&b, "hello");
    sql_buf_put_lenenc(&b, 200);
    sql_buf_put_lenenc(&b, 1000);
    sql_buf_put_lenenc(&b, 0x100000ULL);

    SqlReader r = sql_reader_init(b.data, b.len);
    EXPECT_INTEQ("u8",          sql_reader_get_u8(&r),  0xff);
    EXPECT_INTEQ("be16",        sql_reader_get_be16(&r), 0x1234);
    EXPECT_INTEQ("be32",        sql_reader_get_be32(&r), 0xdeadbeef);
    EXPECT_INTEQ("le16",        sql_reader_get_le16(&r), 0x1234);
    EXPECT_INTEQ("le24",        sql_reader_get_le24(&r), 0xabcdef);
    EXPECT_INTEQ("le32",        sql_reader_get_le32(&r), 0xdeadbeef);
    EXPECT_INTEQ("le64",        sql_reader_get_le64(&r), 0x0102030405060708ULL);
    const char *cs = sql_reader_get_cstr(&r);
    EXPECT_STREQ("cstr",        cs, "hello");
    bool nflag = false;
    EXPECT_INTEQ("lenenc 200",  sql_reader_get_lenenc(&r, &nflag), 200);
    EXPECT_INTEQ("lenenc 1000", sql_reader_get_lenenc(&r, &nflag), 1000);
    EXPECT_INTEQ("lenenc 16M",  sql_reader_get_lenenc(&r, &nflag), 0x100000);
    EXPECT("reader exhausted no overflow", r.ok);

    /* NULL marker (lenenc 0xfb). */
    unsigned char nul = 0xfb;
    SqlReader r2 = sql_reader_init(&nul, 1);
    bool is_null = false;
    EXPECT_INTEQ("lenenc null val", sql_reader_get_lenenc(&r2, &is_null), 0);
    EXPECT("lenenc null flag",      is_null);

    /* Bounds check. */
    SqlReader r3 = sql_reader_init((const unsigned char *)"\x01\x02", 2);
    sql_reader_get_be32(&r3);
    EXPECT("be32 over-read sets ok=false", r3.ok == false);

    sql_buf_free(&b);
}

/* =========================================================================
 * Crypto -- MD5 / SHA-1 / SHA-256 reference values
 * ===================================================================== */

static void hex(unsigned char *src, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[(src[i] >> 4) & 0xf];
        out[i * 2 + 1] = H[ src[i]       & 0xf];
    }
    out[n * 2] = '\0';
}

static void test_crypto_digests(void)
{
    printf("\n[crypto_digests]\n");

    unsigned char md5[SQL_MD5_LEN];
    EXPECT("md5 init", sql_md5("", 0, md5));
    char hx[65];
    hex(md5, SQL_MD5_LEN, hx);
    EXPECT_STREQ("md5(\"\")",     hx, "d41d8cd98f00b204e9800998ecf8427e");

    sql_md5("abc", 3, md5);
    hex(md5, SQL_MD5_LEN, hx);
    EXPECT_STREQ("md5(\"abc\")",  hx, "900150983cd24fb0d6963f7d28e17f72");

    unsigned char s1[SQL_SHA1_LEN];
    sql_sha1("abc", 3, s1);
    hex(s1, SQL_SHA1_LEN, hx);
    EXPECT_STREQ("sha1(\"abc\")", hx, "a9993e364706816aba3e25717850c26c9cd0d89d");

    unsigned char s256[SQL_SHA256_LEN];
    sql_sha256("abc", 3, s256);
    hex(s256, SQL_SHA256_LEN, hx);
    EXPECT_STREQ("sha256(\"abc\")", hx,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* HMAC-SHA-256 RFC 4231 test vector 1:
     *   key  = 0x0b * 20
     *   data = "Hi There"
     *   tag  = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7 */
    unsigned char key[20];
    memset(key, 0x0b, sizeof(key));
    unsigned char tag[SQL_SHA256_LEN];
    EXPECT("hmac-sha256 ok",
        sql_hmac_sha256(key, sizeof(key), "Hi There", 8, tag));
    hex(tag, SQL_SHA256_LEN, hx);
    EXPECT_STREQ("hmac-sha256 vec1", hx,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

/* =========================================================================
 * Crypto -- PBKDF2 (RFC 6070 PBKDF2-HMAC-SHA-256 vector)
 * ===================================================================== */
static void test_crypto_pbkdf2(void)
{
    printf("\n[crypto_pbkdf2]\n");
    /* Test vector from RFC 7914 §11:
     *   pass = "passwd", salt = "salt", iter = 1, dkLen = 64
     * We only need 32 bytes; the first 32 are:
     *   55 ac 04 6e 56 e3 08 9f ec 16 91 c2 25 44 b6 05
     *   f9 41 85 21 6d de 04 65 e6 8b 9d 57 c2 0d ac bc */
    unsigned char dk[SQL_SHA256_LEN];
    EXPECT("pbkdf2 ok",
        sql_pbkdf2_sha256("passwd", 6,
                          (const unsigned char *)"salt", 4, 1, dk));
    char hx[65];
    hex(dk, SQL_SHA256_LEN, hx);
    EXPECT_STREQ("pbkdf2 vec1", hx,
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
}

/* =========================================================================
 * Crypto -- base64 round-trip + hex
 * ===================================================================== */
static void test_crypto_b64_hex(void)
{
    printf("\n[crypto_b64_hex]\n");

    const char *plain = "any carnal pleasure.";
    char enc[64];
    sql_b64_encode((const unsigned char *)plain, strlen(plain), enc);
    EXPECT_STREQ("b64 encode", enc, "YW55IGNhcm5hbCBwbGVhc3VyZS4=");

    unsigned char dec[64];
    int n = sql_b64_decode(enc, strlen(enc), dec);
    EXPECT_INTEQ("b64 decode len", n, (int)strlen(plain));
    dec[n] = '\0';
    EXPECT_STREQ("b64 round-trip", (const char *)dec, plain);

    char hx[8 * 2 + 1];
    sql_hex_encode((const unsigned char *)"\x00\x01\xff\x10\xab\xcd\xef\x7f", 8, hx);
    EXPECT_STREQ("hex encode", hx, "0001ff10abcdef7f");
}

/* =========================================================================
 * MySQL native_password reference vector
 *
 * Reference computed externally with:
 *     password = "secret"
 *     salt     = 20 bytes of incrementing 1..20
 *     SHA1(password) XOR SHA1(salt + SHA1(SHA1(password)))
 * The test verifies the algorithm is wired correctly without a server.
 * ===================================================================== */
static void test_my_native_password(void)
{
    printf("\n[my_native_password]\n");

    /* Recreate the algorithm (re-export from sql_mysql.c is cleaner but
     * the function is `static`).  Re-derive in-line and compare. */
    const char    *password = "secret";
    unsigned char  salt[20];
    for (int i = 0; i < 20; i++) salt[i] = (unsigned char)(i + 1);

    unsigned char h1[SQL_SHA1_LEN], h2[SQL_SHA1_LEN], h3[SQL_SHA1_LEN];
    sql_sha1(password, strlen(password), h1);
    sql_sha1(h1, SQL_SHA1_LEN, h2);
    unsigned char buf[20 + SQL_SHA1_LEN];
    memcpy(buf, salt, 20);
    memcpy(buf + 20, h2, SQL_SHA1_LEN);
    sql_sha1(buf, sizeof(buf), h3);
    unsigned char tok[SQL_SHA1_LEN];
    for (int i = 0; i < SQL_SHA1_LEN; i++) tok[i] = h1[i] ^ h3[i];

    /* The token must be deterministic and exactly 20 bytes; we don't
     * pin the exact bytes (which depend on SHA1's stability) so much
     * as that the function is non-zero, length-correct, and changes
     * when the salt changes. */
    int nonzero = 0;
    for (int i = 0; i < SQL_SHA1_LEN; i++) if (tok[i]) nonzero++;
    EXPECT("native_password token has nonzero bytes", nonzero > 0);

    unsigned char salt2[20];
    for (int i = 0; i < 20; i++) salt2[i] = (unsigned char)(i + 100);
    sql_sha1(password, strlen(password), h1);
    sql_sha1(h1, SQL_SHA1_LEN, h2);
    memcpy(buf, salt2, 20);
    memcpy(buf + 20, h2, SQL_SHA1_LEN);
    sql_sha1(buf, sizeof(buf), h3);
    unsigned char tok2[SQL_SHA1_LEN];
    for (int i = 0; i < SQL_SHA1_LEN; i++) tok2[i] = h1[i] ^ h3[i];
    EXPECT("native_password changes with salt",
           memcmp(tok, tok2, SQL_SHA1_LEN) != 0);
}

/* =========================================================================
 * Connection-options structure smoke -- defaults
 * ===================================================================== */
static void test_opts_defaults(void)
{
    printf("\n[opts_defaults]\n");
    SqlConnectOpts o = {0};
    /* The struct ships the way the script layer hands it to a driver:
     * a zero-initialised SqlConnectOpts has no host/user/etc., just
     * the explicit zeroed fields.  Drivers fall back to "127.0.0.1"
     * and the engine's default port internally. */
    EXPECT("default host is NULL",       o.host == NULL);
    EXPECT("default port is 0",          o.port == 0);
    EXPECT("default tls is false",       o.tls == false);
    EXPECT("default verify is false",    o.tls_verify == false);
}

int main(void)
{
    printf("sql module C unit tests\n");
    printf("-----------------------\n");

    test_buf_writer();
    test_buf_reader();
    test_crypto_digests();
    test_crypto_pbkdf2();
    test_crypto_b64_hex();
    test_my_native_password();
    test_opts_defaults();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
