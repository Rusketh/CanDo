/*
 * modules/smtp/test_smtp.c -- C unit tests for the pure-C helpers in
 * smtp_helpers.h, mime.h, dkim.h, dns.h, spf.h, storage.h.
 *
 * The tests do not link libcando -- they only include the header-only
 * helper files.  This keeps the C-only test pass fast and cross-platform
 * (CI runs it on every push without needing a built libcando).
 *
 * Build:
 *     make -C modules/smtp test
 * which is equivalent to:
 *     cc -std=c11 -DSMTP_MODULE_TEST_BUILD test_smtp.c -o test_smtp -lssl -lcrypto -lresolv
 *
 * Each test is a `static int test_xxx(void)` that returns 0 on pass.
 * The driver counts and prints a summary.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "smtp_helpers.h"
#include "mime.h"
/* dkim/spf/dns are network-dependent; covered by integration tests. */

static int failures = 0;
static int passes   = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

#define EXPECT_STR_EQ(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "  FAIL: %s -- got '%s' expected '%s' (%s:%d)\n", \
                msg, (a), (b), __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

#define RUN(t) do { \
    fprintf(stderr, "test_%s\n", #t); \
    if (test_##t() != 0) failures++; else passes++; \
} while (0)

/* ===================================================================== */

static int test_base64(void)
{
    char out[256];
    size_t w = b64_encode((const uint8_t *)"hello", 5, out, sizeof(out));
    EXPECT(w == 8, "encode length");
    EXPECT_STR_EQ(out, "aGVsbG8=", "encode hello");

    uint8_t bin[16];
    size_t r = b64_decode("aGVsbG8=", 8, bin, sizeof(bin));
    EXPECT(r == 5, "decode length");
    EXPECT(memcmp(bin, "hello", 5) == 0, "decode hello");

    /* Empty roundtrip. */
    w = b64_encode((const uint8_t *)"", 0, out, sizeof(out));
    EXPECT(w == 0, "encode empty");
    /* Padding. */
    w = b64_encode((const uint8_t *)"a", 1, out, sizeof(out));
    EXPECT_STR_EQ(out, "YQ==", "encode 1 byte");
    w = b64_encode((const uint8_t *)"ab", 2, out, sizeof(out));
    EXPECT_STR_EQ(out, "YWI=", "encode 2 bytes");
    return 0;
}

static int test_qp(void)
{
    sb_t out; sb_init(&out);
    /* Encode. */
    qp_encode((const uint8_t *)"hi=there\n", 9, &out);
    EXPECT_STR_EQ(out.data, "hi=3Dthere\r\n", "qp encode");
    sb_free(&out);

    /* Decode. */
    sb_init(&out);
    qp_decode("hi=3Dthere", 10, &out);
    EXPECT_STR_EQ(out.data, "hi=there", "qp decode");
    sb_free(&out);

    /* Soft line break. */
    sb_init(&out);
    qp_decode("hello=\r\nworld", 13, &out);
    EXPECT_STR_EQ(out.data, "helloworld", "qp soft break");
    sb_free(&out);
    return 0;
}

static int test_dot_stuff(void)
{
    sb_t out; sb_init(&out);
    dot_stuff(".hello\nworld\n.\n", 15, &out);
    /* Lines starting with '.' get an extra '.'; LF -> CRLF. */
    EXPECT(strstr(out.data, "..hello") != NULL, "dot stuff dot");
    EXPECT(strstr(out.data, "..\r\n") != NULL, "dot stuff terminator-like");
    sb_free(&out);
    return 0;
}

static int test_address_parse(void)
{
    char name[256], addr[256];
    bool ok;
    ok = parse_one_address("alice@example.com", 17, name, sizeof(name),
                           addr, sizeof(addr));
    EXPECT(ok, "bare addr");
    EXPECT_STR_EQ(name, "", "bare name empty");
    EXPECT_STR_EQ(addr, "alice@example.com", "bare addr value");

    ok = parse_one_address("Alice <alice@example.com>", 25,
                           name, sizeof(name), addr, sizeof(addr));
    EXPECT(ok, "name + addr");
    EXPECT_STR_EQ(name, "Alice", "name");
    EXPECT_STR_EQ(addr, "alice@example.com", "addr");

    ok = parse_one_address("\"Alice, the Great\" <a@b>", 24,
                           name, sizeof(name), addr, sizeof(addr));
    EXPECT(ok, "quoted name");
    EXPECT_STR_EQ(name, "Alice, the Great", "quoted name body");
    EXPECT_STR_EQ(addr, "a@b", "quoted name addr");
    return 0;
}

typedef struct { int n; char items[8][128]; } split_ud_t;

static bool split_collect(const char *s, size_t n, void *u)
{
    split_ud_t *p = (split_ud_t *)u;
    if (p->n < 8) {
        size_t k = n < 127 ? n : 127;
        memcpy(p->items[p->n], s, k);
        p->items[p->n++][k] = '\0';
    }
    return true;
}

static int test_address_list_split(void)
{
    split_ud_t ud = { 0 };
    const char *list = "a@b, \"C, D\" <c@d>, e@f";
    split_addr_list(list, strlen(list), split_collect, &ud);
    EXPECT(ud.n == 3, "split count");
    EXPECT(strstr(ud.items[1], "C, D") != NULL, "quoted comma respected");
    return 0;
}

static int test_rfc2047(void)
{
    /* Q-encoding round-trip. */
    sb_t out; sb_init(&out);
    rfc2047_encode_q("Schöne", 7, &out);
    EXPECT(strstr(out.data, "=?UTF-8?Q?") == out.data, "encoded prefix");
    sb_free(&out);

    /* Decoder: Q. */
    sb_init(&out);
    rfc2047_decode("=?UTF-8?Q?Sch=C3=B6ne?=", 23, &out);
    EXPECT(strstr(out.data, "Sch") != NULL, "decoded contains Sch");
    sb_free(&out);

    /* Decoder: B. */
    sb_init(&out);
    rfc2047_decode("=?UTF-8?B?aGVsbG8=?=", 20, &out);
    EXPECT_STR_EQ(out.data, "hello", "B decoded");
    sb_free(&out);

    return 0;
}

static int test_mime_parse(void)
{
    const char *raw =
        "From: Alice <a@x>\r\n"
        "To: Bob <b@y>\r\n"
        "Subject: Hi\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "hello world\r\n";
    mime_part_t *p = mime_parse((const uint8_t *)raw, strlen(raw));
    EXPECT(p != NULL, "parse ok");
    EXPECT_STR_EQ(p->content_type, "text/plain", "content-type");
    EXPECT(p->body_len == 13 || p->body_len == 11, "body");
    /* find_text_part */
    const mime_part_t *t = find_text_part(p, false);
    EXPECT(t != NULL, "text/plain leaf");
    mime_part_free(p);
    return 0;
}

static int test_mime_multipart(void)
{
    const char *raw =
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/alternative; boundary=\"X\"\r\n"
        "\r\n"
        "--X\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "plain part\r\n"
        "--X\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<p>html part</p>\r\n"
        "--X--\r\n";
    mime_part_t *p = mime_parse((const uint8_t *)raw, strlen(raw));
    EXPECT(p != NULL, "parse ok");
    EXPECT(p->n_children == 2, "two children");
    const mime_part_t *t = find_text_part(p, false);
    const mime_part_t *h = find_text_part(p, true);
    EXPECT(t != NULL, "plain leaf");
    EXPECT(h != NULL, "html leaf");
    EXPECT(t->body && memcmp(t->body, "plain part", 10) == 0, "plain body");
    EXPECT(h->body && memcmp(h->body, "<p>html part</p>", 16) == 0, "html body");
    mime_part_free(p);
    return 0;
}

static int test_mime_build_roundtrip(void)
{
    mime_build_t b = { 0 };
    b.from = "Alice <a@x.com>";
    b.to   = "Bob <b@y.com>";
    b.subject = "Hi there";
    b.text = "Hello!\n";
    b.html = "<p>Hello!</p>";
    sb_t out; sb_init(&out);
    bool ok = mime_build(&b, &out);
    EXPECT(ok, "build ok");
    EXPECT(strstr(out.data, "Subject: Hi there") != NULL, "subject");
    EXPECT(strstr(out.data, "multipart/alternative") != NULL, "multipart/alt");
    /* parse back */
    mime_part_t *p = mime_parse((const uint8_t *)out.data, out.len);
    EXPECT(p != NULL, "reparse ok");
    const mime_part_t *t = find_text_part(p, false);
    const mime_part_t *h = find_text_part(p, true);
    EXPECT(t != NULL && h != NULL, "both leaves");
    /* Note: body decoder yields the canonicalised form; we only check
     * substrings exist (CRLF normalisation may differ). */
    EXPECT(t->body && strstr((const char *)t->body, "Hello!"), "text body");
    EXPECT(h->body && strstr((const char *)h->body, "<p>Hello!</p>"), "html body");
    mime_part_free(p);
    sb_free(&out);
    return 0;
}

static int test_header_safety(void)
{
    EXPECT(header_value_safe("OK value", 8), "safe value");
    EXPECT(!header_value_safe("bad\r\nX-Inject: yes", 18), "rejects CRLF");
    EXPECT(!header_value_safe("bad\nX-Inject: yes", 17), "rejects LF");
    return 0;
}

int main(void)
{
    RUN(base64);
    RUN(qp);
    RUN(dot_stuff);
    RUN(address_parse);
    RUN(address_list_split);
    RUN(rfc2047);
    RUN(mime_parse);
    RUN(mime_multipart);
    RUN(mime_build_roundtrip);
    RUN(header_safety);

    fprintf(stderr, "\nSMTP module C tests: %d passed, %d failed\n",
            passes, failures);
    return failures == 0 ? 0 : 1;
}
