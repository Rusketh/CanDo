/*
 * tests/test_jit_hot.c -- Unit tests for source/jit/hot.{h,c}.
 *
 * Compile via Makefile:
 *   make test_jit_hot
 *
 * Tests cover:
 *   - init/destroy with default and explicit thresholds
 *   - cando_hot_hit basic counting + threshold trigger
 *   - one-shot trigger semantics (auto-blacklist on threshold)
 *   - distinct PCs are tracked independently
 *   - cando_hot_blacklist explicit + idempotency
 *   - bucket growth across many distinct PCs
 *   - cando_hot_count / cando_hot_is_blacklisted accessors
 *   - threshold setter/getter
 */

#include "common.h"
#include "hot.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0, g_pass = 0, g_fail = 0;

#define EXPECT(cond) do { \
    g_run++; \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define TEST(name) static void name(void)

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-44s ", name);
    fflush(stdout);
    int before = g_fail;
    fn();
    printf(g_fail == before ? "OK\n" : "FAILED\n");
}

/* Synthetic PC values: any non-NULL pointer works.  We use small
 * integer offsets cast to (const u8*) so the test is reproducible
 * without needing a real chunk. */
static const u8 *fake_pc(uintptr_t v) { return (const u8 *)v; }

/* ----------------------------------------------------------------------- */

TEST(test_init_default_threshold) {
    CandoHotTable t;
    cando_hot_table_init(&t, 0);

    EXPECT_EQ(cando_hot_get_threshold(&t), CANDO_HOT_DEFAULT_THRESHOLD);
    EXPECT_EQ(cando_hot_entry_count(&t), 0u);
    EXPECT_EQ(cando_hot_blacklist_count(&t), 0u);

    cando_hot_table_destroy(&t);
}

TEST(test_init_explicit_threshold) {
    CandoHotTable t;
    cando_hot_table_init(&t, 5);
    EXPECT_EQ(cando_hot_get_threshold(&t), 5u);
    cando_hot_table_destroy(&t);
}

TEST(test_hit_below_threshold) {
    CandoHotTable t;
    cando_hot_table_init(&t, 5);

    const u8 *pc = fake_pc(0x1000);
    /* First 4 hits: no trigger. */
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_EQ(cando_hot_count(&t, pc), 4u);
    EXPECT_FALSE(cando_hot_is_blacklisted(&t, pc));
    EXPECT_EQ(cando_hot_entry_count(&t), 1u);

    cando_hot_table_destroy(&t);
}

TEST(test_hit_threshold_triggers_once) {
    CandoHotTable t;
    cando_hot_table_init(&t, 3);

    const u8 *pc = fake_pc(0x2000);
    EXPECT_FALSE(cando_hot_hit(&t, pc));   /* count = 1 */
    EXPECT_FALSE(cando_hot_hit(&t, pc));   /* count = 2 */
    EXPECT_TRUE (cando_hot_hit(&t, pc));   /* count = 3, threshold reached */
    EXPECT_TRUE (cando_hot_is_blacklisted(&t, pc));
    EXPECT_EQ(cando_hot_blacklist_count(&t), 1u);

    /* Subsequent hits do NOT re-trigger.  Counter still bumps so the
     * "how often did this site fire after blacklisting" diagnostic
     * stays accurate. */
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_FALSE(cando_hot_hit(&t, pc));
    EXPECT_EQ(cando_hot_count(&t, pc), 5u);
    EXPECT_EQ(cando_hot_blacklist_count(&t), 1u);  /* not double-counted */

    cando_hot_table_destroy(&t);
}

TEST(test_distinct_pcs_independent) {
    CandoHotTable t;
    cando_hot_table_init(&t, 2);

    const u8 *a = fake_pc(0x3000);
    const u8 *b = fake_pc(0x3008);

    EXPECT_FALSE(cando_hot_hit(&t, a));       /* a:1 */
    EXPECT_FALSE(cando_hot_hit(&t, b));       /* b:1 */
    EXPECT_TRUE (cando_hot_hit(&t, a));       /* a:2 trigger */
    EXPECT_TRUE (cando_hot_is_blacklisted(&t, a));
    EXPECT_FALSE(cando_hot_is_blacklisted(&t, b));
    EXPECT_TRUE (cando_hot_hit(&t, b));       /* b:2 trigger (independent) */

    EXPECT_EQ(cando_hot_entry_count(&t), 2u);
    EXPECT_EQ(cando_hot_blacklist_count(&t), 2u);

    cando_hot_table_destroy(&t);
}

TEST(test_explicit_blacklist) {
    CandoHotTable t;
    cando_hot_table_init(&t, 100);

    const u8 *pc = fake_pc(0x4000);
    cando_hot_blacklist(&t, pc);
    EXPECT_TRUE(cando_hot_is_blacklisted(&t, pc));
    EXPECT_EQ(cando_hot_blacklist_count(&t), 1u);

    /* Hits never trigger because the PC is already blacklisted. */
    for (int i = 0; i < 200; i++)
        EXPECT_FALSE(cando_hot_hit(&t, pc));

    /* Idempotent. */
    cando_hot_blacklist(&t, pc);
    EXPECT_EQ(cando_hot_blacklist_count(&t), 1u);

    cando_hot_table_destroy(&t);
}

TEST(test_bucket_growth) {
    CandoHotTable t;
    cando_hot_table_init(&t, 100);   /* high so nothing triggers */

    /* Insert 200 distinct PCs.  Initial bucket count is 64; growth
     * fires at 75% load (48 entries) and again at 96, bringing the
     * table to at least 256 buckets. */
    const u32 N = 200;
    for (u32 i = 0; i < N; i++) {
        const u8 *pc = fake_pc(0x10000 + i * 16);
        EXPECT_FALSE(cando_hot_hit(&t, pc));
    }
    EXPECT_EQ(cando_hot_entry_count(&t), N);

    /* All counts should be 1 -- the growth must preserve every entry. */
    u32 ok = 0;
    for (u32 i = 0; i < N; i++) {
        const u8 *pc = fake_pc(0x10000 + i * 16);
        if (cando_hot_count(&t, pc) == 1u) ok++;
    }
    EXPECT_EQ(ok, N);

    cando_hot_table_destroy(&t);
}

TEST(test_count_absent_pc) {
    CandoHotTable t;
    cando_hot_table_init(&t, 0);

    EXPECT_EQ(cando_hot_count(&t, fake_pc(0xDEAD)), 0u);
    EXPECT_FALSE(cando_hot_is_blacklisted(&t, fake_pc(0xDEAD)));
    EXPECT_EQ(cando_hot_entry_count(&t), 0u);  /* read-only access: no insert */

    cando_hot_table_destroy(&t);
}

TEST(test_set_threshold) {
    CandoHotTable t;
    cando_hot_table_init(&t, 10);
    cando_hot_set_threshold(&t, 3);
    EXPECT_EQ(cando_hot_get_threshold(&t), 3u);
    /* 0 falls back to default. */
    cando_hot_set_threshold(&t, 0);
    EXPECT_EQ(cando_hot_get_threshold(&t), CANDO_HOT_DEFAULT_THRESHOLD);
    cando_hot_table_destroy(&t);
}

TEST(test_null_safety) {
    /* All public functions tolerate NULL table or NULL pc. */
    EXPECT_FALSE(cando_hot_hit(NULL, fake_pc(0x1)));
    EXPECT_EQ   (cando_hot_count(NULL, fake_pc(0x1)), 0u);
    EXPECT_FALSE(cando_hot_is_blacklisted(NULL, fake_pc(0x1)));
    EXPECT_EQ   (cando_hot_get_threshold(NULL), 0u);
    EXPECT_EQ   (cando_hot_entry_count(NULL), 0u);
    EXPECT_EQ   (cando_hot_blacklist_count(NULL), 0u);
    cando_hot_blacklist(NULL, fake_pc(0x1));   /* must not crash */
    cando_hot_set_threshold(NULL, 5);
    cando_hot_table_destroy(NULL);
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("\n=== test_jit_hot ===\n");

    run_test("init default threshold",        test_init_default_threshold);
    run_test("init explicit threshold",       test_init_explicit_threshold);
    run_test("hits below threshold",          test_hit_below_threshold);
    run_test("threshold triggers exactly once", test_hit_threshold_triggers_once);
    run_test("distinct PCs independent",      test_distinct_pcs_independent);
    run_test("explicit blacklist + idempotent", test_explicit_blacklist);
    run_test("bucket growth preserves entries", test_bucket_growth);
    run_test("count on absent PC",            test_count_absent_pc);
    run_test("set / get threshold",           test_set_threshold);
    run_test("NULL safety",                   test_null_safety);

    printf("\n=== Results: %d/%d passed ===\n", g_pass, g_run);
    return g_fail > 0 ? 1 : 0;
}
