/*
 * modules/ldap/test_ldap.c -- Unit tests for the LDAP / Active Directory
 * module's pure-C helpers.
 *
 * Build with the per-module Makefile:
 *
 *     make -C modules/ldap test
 *
 * The tests cover:
 *   - scope-name parsing (string -> LDAP_SCOPE_*)
 *   - modification op-name parsing (string -> LDAP_MOD_*)
 *   - RDN extraction (handling of escaped commas)
 *   - the ldap_err2string round-trip used by the error reporter
 *
 * The full network-facing surface (connect / bind / search / add / modify
 * / delete / move / rename) is exercised by the script-level integration
 * test in tests/integration/scripts/ldap_module.cdo, which runs against
 * the binary module loaded through include().
 */

#include "ldap_helpers.h"
#include "ldap_adcrypt.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <winldap.h>
#else
#  include <ldap.h>
#endif

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(name, cond)                                                  \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  PASS  %s\n", name);                                   \
            g_pass++;                                                       \
        } else {                                                            \
            printf("  FAIL  %s  (line %d)\n", name, __LINE__);              \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static void test_parse_scope(void)
{
    int s = -1;
    EXPECT("scope: base",
        ldap_helpers_parse_scope("base", &s) == 1 && s == LDAP_SCOPE_BASE);
    s = -1;
    EXPECT("scope: one",
        ldap_helpers_parse_scope("one", &s) == 1 && s == LDAP_SCOPE_ONELEVEL);
    s = -1;
    EXPECT("scope: onelevel",
        ldap_helpers_parse_scope("onelevel", &s) == 1 && s == LDAP_SCOPE_ONELEVEL);
    s = -1;
    EXPECT("scope: sub",
        ldap_helpers_parse_scope("sub", &s) == 1 && s == LDAP_SCOPE_SUBTREE);
    s = -1;
    EXPECT("scope: subtree",
        ldap_helpers_parse_scope("subtree", &s) == 1 && s == LDAP_SCOPE_SUBTREE);

    /* Unknown name -- must not modify *out and must return 0. */
    int sentinel = 0xCAFE;
    EXPECT("scope: unknown returns 0",
        ldap_helpers_parse_scope("bogus", &sentinel) == 0);
    EXPECT("scope: unknown leaves out untouched",
        sentinel == 0xCAFE);

    /* NULL inputs must not crash and must return 0. */
    EXPECT("scope: NULL name",  ldap_helpers_parse_scope(NULL, &sentinel) == 0);
    EXPECT("scope: NULL out",   ldap_helpers_parse_scope("sub", NULL) == 0);
}

static void test_parse_mod_op(void)
{
    int op = -1;
    EXPECT("modop: add",
        ldap_helpers_parse_mod_op("add", &op) == 1 && op == LDAP_MOD_ADD);
    op = -1;
    EXPECT("modop: replace",
        ldap_helpers_parse_mod_op("replace", &op) == 1
            && op == LDAP_MOD_REPLACE);
    op = -1;
    EXPECT("modop: delete",
        ldap_helpers_parse_mod_op("delete", &op) == 1
            && op == LDAP_MOD_DELETE);

    int sentinel = 0xBEEF;
    EXPECT("modop: unknown returns 0",
        ldap_helpers_parse_mod_op("modify", &sentinel) == 0);
    EXPECT("modop: unknown leaves out untouched",
        sentinel == 0xBEEF);
}

static void test_extract_rdn(void)
{
    char buf[256];

    EXPECT("rdn: simple",
        ldap_helpers_extract_rdn("cn=Ada,ou=Users,dc=ex,dc=com",
                                  buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Ada") == 0);

    /* Comma is escaped with a backslash -- must not split there. */
    EXPECT("rdn: escaped comma",
        ldap_helpers_extract_rdn("cn=Doe\\, Jane,ou=Users,dc=ex,dc=com",
                                  buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Doe\\, Jane") == 0);

    /* No comma at all -- the whole string is the RDN. */
    EXPECT("rdn: no comma",
        ldap_helpers_extract_rdn("cn=Solo", buf, sizeof(buf)) == 1
            && strcmp(buf, "cn=Solo") == 0);

    /* Buffer overflow -- function must reject. */
    char tiny[4];
    EXPECT("rdn: buffer too small",
        ldap_helpers_extract_rdn("cn=Ada,dc=x", tiny, sizeof(tiny)) == 0);

    /* NULL inputs -- must return 0 without crashing. */
    EXPECT("rdn: NULL dn",  ldap_helpers_extract_rdn(NULL, buf, sizeof(buf)) == 0);
    EXPECT("rdn: NULL buf", ldap_helpers_extract_rdn("cn=x", NULL, 16) == 0);
    EXPECT("rdn: zero len", ldap_helpers_extract_rdn("cn=x", buf, 0) == 0);
}

static void test_err2string(void)
{
#if defined(_WIN32) || defined(_WIN64)
    const char *msg = (const char *)ldap_err2string(LDAP_SUCCESS);
#else
    const char *msg = ldap_err2string(LDAP_SUCCESS);
#endif
    EXPECT("err2string: SUCCESS not NULL",   msg != NULL);
    EXPECT("err2string: SUCCESS is non-empty", msg && msg[0] != '\0');

#if defined(_WIN32) || defined(_WIN64)
    const char *err = (const char *)ldap_err2string(LDAP_NO_SUCH_OBJECT);
#else
    const char *err = ldap_err2string(LDAP_NO_SUCH_OBJECT);
#endif
    EXPECT("err2string: NO_SUCH_OBJECT not NULL", err != NULL);
}

static void test_escape_filter(void)
{
    char buf[256];
    long n;

    n = ldap_helpers_escape_filter("plain", 5, buf, sizeof(buf));
    EXPECT("filter: plain", n == 5 && strcmp(buf, "plain") == 0);

    /* RFC 4515 example: each special encoded as \xx */
    n = ldap_helpers_escape_filter("a*b", 3, buf, sizeof(buf));
    EXPECT("filter: star", n == 5 && strcmp(buf, "a\\2ab") == 0);

    n = ldap_helpers_escape_filter("(", 1, buf, sizeof(buf));
    EXPECT("filter: open paren", n == 3 && strcmp(buf, "\\28") == 0);

    n = ldap_helpers_escape_filter(")", 1, buf, sizeof(buf));
    EXPECT("filter: close paren", n == 3 && strcmp(buf, "\\29") == 0);

    n = ldap_helpers_escape_filter("a\\b", 3, buf, sizeof(buf));
    EXPECT("filter: backslash", n == 5 && strcmp(buf, "a\\5cb") == 0);

    /* Embedded NUL is encoded \00 -- byte safe. */
    char in_nul[3] = { 'a', '\0', 'b' };
    n = ldap_helpers_escape_filter(in_nul, 3, buf, sizeof(buf));
    EXPECT("filter: NUL byte", n == 5 && memcmp(buf, "a\\00b", 5) == 0);

    /* Sizing query: buflen == 0 returns required count without writing. */
    n = ldap_helpers_escape_filter("(*)", 3, NULL, 0);
    EXPECT("filter: sizing query", n == 9);

    /* Buffer too small. */
    n = ldap_helpers_escape_filter("a*b", 3, buf, 4);
    EXPECT("filter: buffer too small", n == -1);
}

static void test_escape_dn(void)
{
    char buf[256];
    long n;

    n = ldap_helpers_escape_dn("plain", 5, buf, sizeof(buf));
    EXPECT("dn: plain", n == 5 && strcmp(buf, "plain") == 0);

    n = ldap_helpers_escape_dn("Doe, Jane", 9, buf, sizeof(buf));
    EXPECT("dn: comma", n == 10 && strcmp(buf, "Doe\\, Jane") == 0);

    n = ldap_helpers_escape_dn("a+b", 3, buf, sizeof(buf));
    EXPECT("dn: plus", n == 4 && strcmp(buf, "a\\+b") == 0);

    n = ldap_helpers_escape_dn("a\"b", 3, buf, sizeof(buf));
    EXPECT("dn: quote", n == 4 && strcmp(buf, "a\\\"b") == 0);

    n = ldap_helpers_escape_dn(" leading", 8, buf, sizeof(buf));
    EXPECT("dn: leading space", n == 9 && strcmp(buf, "\\ leading") == 0);

    n = ldap_helpers_escape_dn("trailing ", 9, buf, sizeof(buf));
    EXPECT("dn: trailing space", n == 10 && strcmp(buf, "trailing\\ ") == 0);

    n = ldap_helpers_escape_dn("#hash", 5, buf, sizeof(buf));
    EXPECT("dn: leading hash", n == 6 && strcmp(buf, "\\#hash") == 0);

    char in_nul[3] = { 'a', '\0', 'b' };
    n = ldap_helpers_escape_dn(in_nul, 3, buf, sizeof(buf));
    EXPECT("dn: NUL", n == 5 && memcmp(buf, "a\\00b", 5) == 0);
}

static void test_md5(void)
{
    /* RFC 1321 test vectors */
    uint8_t out[16];

    adc_md5((const uint8_t *)"", 0, out);
    /* MD5("") = d41d8cd98f00b204e9800998ecf8427e */
    static const uint8_t v0[16] = {
        0xd4,0x1d,0x8c,0xd9,0x8f,0x00,0xb2,0x04,
        0xe9,0x80,0x09,0x98,0xec,0xf8,0x42,0x7e };
    EXPECT("md5: empty string", memcmp(out, v0, 16) == 0);

    adc_md5((const uint8_t *)"abc", 3, out);
    /* MD5("abc") = 900150983cd24fb0d6963f7d28e17f72 */
    static const uint8_t v1[16] = {
        0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
        0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72 };
    EXPECT("md5: 'abc'", memcmp(out, v1, 16) == 0);

    /* RFC 1321 alpha-num vector (note: UPPERCASE first). */
    static const char *long_in =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    adc_md5((const uint8_t *)long_in, strlen(long_in), out);
    static const uint8_t v2[16] = {
        0xd1,0x74,0xab,0x98,0xd2,0x77,0xd9,0xf5,
        0xa5,0x61,0x1c,0x2c,0x9f,0x41,0x9d,0x9f };
    EXPECT("md5: alnum", memcmp(out, v2, 16) == 0);
}

static void test_rc4(void)
{
    /* Vector from RFC 6229 §2: Key=0x0102030405, Plaintext=0..., Keystream */
    static const uint8_t key[5] = { 0x01,0x02,0x03,0x04,0x05 };
    static const uint8_t expected_ks[16] = {
        0xb2,0x39,0x63,0x05,0xf0,0x3d,0xc0,0x27,
        0xcc,0xc3,0x52,0x4a,0x0a,0x11,0x18,0xa8 };
    uint8_t in[16] = { 0 };
    uint8_t out[16];
    ADC_RC4_CTX rc4;
    adc_rc4_init(&rc4, key, sizeof(key));
    adc_rc4_xor(&rc4, in, out, sizeof(in));
    EXPECT("rc4: RFC 6229 vector", memcmp(out, expected_ks, 16) == 0);

    /* Round-trip: encrypt then decrypt produces original. */
    static const uint8_t key2[] = "secret";
    static const char    msg[]  = "hello world";
    uint8_t enc[sizeof(msg)];
    uint8_t dec[sizeof(msg)];
    adc_rc4_init(&rc4, key2, sizeof(key2) - 1);
    adc_rc4_xor(&rc4, (const uint8_t *)msg, enc, sizeof(msg) - 1);
    adc_rc4_init(&rc4, key2, sizeof(key2) - 1);
    adc_rc4_xor(&rc4, enc, dec, sizeof(msg) - 1);
    EXPECT("rc4: round-trip", memcmp(dec, msg, sizeof(msg) - 1) == 0);
}

static void test_utf16le_to_utf8(void)
{
    char out[64];
    long n;

    /* "ABC" in UTF-16LE */
    static const uint8_t abc[] = { 0x41,0x00, 0x42,0x00, 0x43,0x00 };
    n = adc_utf16le_to_utf8(abc, sizeof(abc), out, sizeof(out));
    EXPECT("utf16le: ASCII", n == 3 && memcmp(out, "ABC", 3) == 0);

    /* "ñ" U+00F1 in UTF-16LE -> 2 UTF-8 bytes 0xC3 0xB1 */
    static const uint8_t nya[] = { 0xF1,0x00 };
    n = adc_utf16le_to_utf8(nya, sizeof(nya), out, sizeof(out));
    EXPECT("utf16le: 2-byte UTF-8",
        n == 2 && (uint8_t)out[0] == 0xC3 && (uint8_t)out[1] == 0xB1);

    /* Odd input length is invalid. */
    n = adc_utf16le_to_utf8(abc, 5, out, sizeof(out));
    EXPECT("utf16le: odd length rejected", n == -1);
}

static void test_decode_reversible_roundtrip(void)
{
    /* Encrypt a known UTF-16LE password with a known key, then verify the
     * decoder turns it back into the original UTF-8.  This exercises the
     * full RC4 + UTF-16LE -> UTF-8 path the native uses. */
    static const uint8_t key[] = "syskey-derived-key";
    static const char    pw[]  = "P@ssw0rd!";

    /* Build UTF-16LE plaintext (ASCII -> two-byte LE). */
    size_t pw_len = sizeof(pw) - 1;
    uint8_t plain[64];
    for (size_t i = 0; i < pw_len; i++) {
        plain[i*2]   = (uint8_t)pw[i];
        plain[i*2+1] = 0x00;
    }
    size_t plen = pw_len * 2;

    /* Encrypt. */
    uint8_t blob[64];
    ADC_RC4_CTX rc4;
    adc_rc4_init(&rc4, key, sizeof(key) - 1);
    adc_rc4_xor(&rc4, plain, blob, plen);

    /* Decrypt with the helper. */
    uint8_t recovered[64];
    adc_decode_reversible(blob, plen, key, sizeof(key) - 1, recovered);
    EXPECT("decode_reversible: round-trip plaintext bytes",
        memcmp(recovered, plain, plen) == 0);

    /* Convert UTF-16LE -> UTF-8. */
    char out[64];
    long n = adc_utf16le_to_utf8(recovered, plen, out, sizeof(out));
    EXPECT("decode_reversible: UTF-8 result matches",
        n == (long)pw_len && memcmp(out, pw, pw_len) == 0);
}

int main(void)
{
    printf("ldap module unit tests\n");
    printf("----------------------\n");

    test_parse_scope();
    test_parse_mod_op();
    test_extract_rdn();
    test_err2string();
    test_escape_filter();
    test_escape_dn();
    test_md5();
    test_rc4();
    test_utf16le_to_utf8();
    test_decode_reversible_roundtrip();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
