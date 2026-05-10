/*
 * tests/test_jit_mcode.c -- Unit tests for source/jit/mcode.{h,c}.
 *
 * Compile via Makefile:
 *   make test_jit_mcode
 *
 * Tests cover:
 *   - alloc / free lifecycle (zero size, small size, page rounding)
 *   - write up to capacity, refuse overflow
 *   - finalize flips to executable; written bytes are callable
 *   - finalize twice is idempotent; write after finalize fails
 *   - free is idempotent
 *   - NULL safety on every entry point
 *
 * The "callable" test emits the smallest possible x86_64 stub that
 * returns a known value (mov $42, %eax; ret) so we exercise the full
 * mmap -> write -> mprotect -> jmp roundtrip without depending on
 * any codegen machinery.
 */

#include "common.h"
#include "mcode.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0, g_pass = 0, g_fail = 0;

#define EXPECT(cond) do { \
    g_run++; \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define EXPECT_TRUE(c)  EXPECT(c)
#define EXPECT_FALSE(c) EXPECT(!(c))
#define EXPECT_EQ(a,b)  EXPECT((a) == (b))

static void run_test(const char *name, void (*fn)(void)) {
    int p0 = g_pass, f0 = g_fail;
    fn();
    int dp = g_pass - p0, df = g_fail - f0;
    printf("  %-40s %s (%d/%d)\n", name,
           df == 0 ? "OK" : "FAIL", dp, dp + df);
}

/* ----------------------------------------------------------------------- */

static void test_alloc_zero(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 0));
    EXPECT_EQ(m.base, NULL);
    EXPECT_EQ(m.size, 0u);
    cando_mcode_free(&m);
}

static void test_alloc_small_rounds_to_page(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 1));
    EXPECT(m.base != NULL);
    EXPECT(m.size >= 4096u);     /* page-rounded */
    EXPECT_EQ(m.written, 0u);
    EXPECT_EQ(m.finalized, 0);
    cando_mcode_free(&m);
}

static void test_write_up_to_capacity(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 16));
    u8 buf[16] = {0};
    EXPECT_TRUE(cando_mcode_write(&m, buf, 16));
    EXPECT_EQ(m.written, 16u);
    /* Buffer is page-rounded so 17 fits too; what we want to verify
     * is that exactly-up-to-size works, not that we can't go past. */
    cando_mcode_free(&m);
}

static void test_write_overflow_refused(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 4));   /* rounds to 4096 */
    u8 buf[4097] = {0};
    /* The page-rounding makes a one-byte alloc into 4096 bytes; a
     * 4097-byte write should still overflow. */
    EXPECT_FALSE(cando_mcode_write(&m, buf, 4097));
    EXPECT_EQ(m.written, 0u);
    cando_mcode_free(&m);
}

static void test_callable_stub(void) {
    /* x86_64: B8 2A 00 00 00   mov $42, %eax
     *         C3                ret */
    static const u8 stub[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00,
        0xC3
    };
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, sizeof(stub)));
    EXPECT_TRUE(cando_mcode_write(&m, stub, sizeof(stub)));
    EXPECT_TRUE(cando_mcode_finalize(&m));

    int (*fn)(void) = (int (*)(void))(uintptr_t)m.base;
    EXPECT_EQ(fn(), 42);

    cando_mcode_free(&m);
}

static void test_finalize_idempotent(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 8));
    u8 nop = 0x90;
    EXPECT_TRUE(cando_mcode_write(&m, &nop, 1));
    EXPECT_TRUE(cando_mcode_finalize(&m));
    EXPECT_TRUE(cando_mcode_finalize(&m));   /* second call is OK */
    /* Write after finalize must fail (mapping is r-x, not r-w). */
    EXPECT_FALSE(cando_mcode_write(&m, &nop, 1));
    cando_mcode_free(&m);
}

static void test_free_idempotent(void) {
    CandoMCode m;
    EXPECT_TRUE(cando_mcode_alloc(&m, 16));
    cando_mcode_free(&m);
    cando_mcode_free(&m);   /* second call no-ops */
    EXPECT_EQ(m.base, NULL);
}

static void test_null_safety(void) {
    EXPECT_FALSE(cando_mcode_alloc(NULL, 16));
    u8 b = 0;
    EXPECT_FALSE(cando_mcode_write(NULL, &b, 1));
    EXPECT_FALSE(cando_mcode_finalize(NULL));
    cando_mcode_free(NULL);   /* must not crash */
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("\n=== test_jit_mcode ===\n");

    run_test("alloc(0) returns empty mapping",   test_alloc_zero);
    run_test("alloc(1) page-rounds",             test_alloc_small_rounds_to_page);
    run_test("write up to capacity",             test_write_up_to_capacity);
    run_test("write overflow refused",           test_write_overflow_refused);
    run_test("emit + finalize + call stub",      test_callable_stub);
    run_test("finalize idempotent",              test_finalize_idempotent);
    run_test("free idempotent",                  test_free_idempotent);
    run_test("NULL safety",                      test_null_safety);

    printf("\n=== Results: %d/%d passed ===\n", g_pass, g_run);
    return g_fail > 0 ? 1 : 0;
}
