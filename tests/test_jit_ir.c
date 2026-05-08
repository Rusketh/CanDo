/*
 * tests/test_jit_ir.c -- Unit tests for source/jit/ir.{h,c}.
 *
 * Compile via Makefile:
 *   make test_jit_ir
 *
 * Tests cover:
 *   - IR container init/destroy/reset (NOP sentinel, capacity growth)
 *   - cando_ir_emit + cando_ir_get_ins round-trip
 *   - cando_ir_const + cando_ir_get_const + dedup of numbers/strings
 *   - cando_ir_emit_knum convenience helper
 *   - IRRef constant-flag macros (IRREF_K / IRREF_KIDX / IRREF_IS_K)
 *   - cando_ir_op_name / cando_ir_type_name name tables
 *   - cando_ir_dump produces non-empty output
 */

#include "common.h"
#include "value.h"
#include "ir.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0, g_pass = 0, g_fail = 0;

#define EXPECT(cond) do { \
    g_run++; \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))
#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)
#define TEST(name) static void name(void)

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-44s ", name);
    fflush(stdout);
    int before = g_fail;
    fn();
    printf(g_fail == before ? "OK\n" : "FAILED\n");
}

/* ----------------------------------------------------------------------- */

TEST(test_ir_init_destroy) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    /* ir[0] is the IR_NOP sentinel; ir_count starts at 1 so the first
     * real emit lands at index 1.  This guarantees a zeroed IRRef
     * dereferences to a harmless NOP, not a stale instruction. */
    EXPECT_EQ(t.ir_count, 1u);
    EXPECT_TRUE(t.ir != NULL);
    EXPECT_EQ((IROp)t.ir[0].op, IR_NOP);

    cando_trace_ir_destroy(&t);
    EXPECT_EQ(t.ir_count, 0u);
    EXPECT_TRUE(t.ir == NULL);
}

TEST(test_ir_emit_basic) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    IRRef a = cando_ir_emit(&t, IR_KNUM, IRT_NUM, 0, IRREF_K(0), 0);
    IRRef b = cando_ir_emit(&t, IR_KNUM, IRT_NUM, 0, IRREF_K(1), 0);
    IRRef sum = cando_ir_emit(&t, IR_ADD, IRT_NUM, 0, a, b);

    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(sum, 3u);
    EXPECT_EQ(t.ir_count, 4u);

    const IRIns *ins = cando_ir_get_ins(&t, sum);
    EXPECT_TRUE(ins != NULL);
    EXPECT_EQ((IROp)ins->op, IR_ADD);
    EXPECT_EQ((IRType)ins->type, IRT_NUM);
    EXPECT_EQ(ins->op1, a);
    EXPECT_EQ(ins->op2, b);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_capacity_growth) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    /* Emit enough instructions to force at least one realloc. */
    const u32 N = 500;
    for (u32 i = 0; i < N; i++) {
        IRRef r = cando_ir_emit(&t, IR_NOP, IRT_VOID, 0, 0, 0);
        EXPECT_EQ(r, i + 1u);
    }
    EXPECT_EQ(t.ir_count, N + 1u);
    EXPECT_TRUE(t.ir_cap >= N + 1u);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_const_pool_number_dedup) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    IRRef k1 = cando_ir_const(&t, cando_number(3.14));
    IRRef k2 = cando_ir_const(&t, cando_number(3.14));
    IRRef k3 = cando_ir_const(&t, cando_number(2.71));

    EXPECT_TRUE(IRREF_IS_K(k1));
    EXPECT_TRUE(IRREF_IS_K(k2));
    EXPECT_TRUE(IRREF_IS_K(k3));
    EXPECT_EQ(k1, k2);              /* deduplicated */
    EXPECT_NEQ(k1, k3);             /* distinct value */
    EXPECT_EQ(t.const_count, 2u);

    /* Round-trip the value back out. */
    CandoValue v1 = cando_ir_get_const(&t, k1);
    EXPECT_TRUE(cando_is_number(v1));
    EXPECT_EQ(cando_as_number(v1), 3.14);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_const_pool_string_dedup) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    /* Two distinct CandoString allocations with the same bytes must
     * dedup -- the recorder may interleave constants from many call
     * sites and we don't want the pool to balloon. */
    CandoString *s1 = cando_string_new("hello", 5);
    CandoString *s2 = cando_string_new("hello", 5);
    CandoString *s3 = cando_string_new("world", 5);

    IRRef k1 = cando_ir_const(&t, cando_string_value(s1));
    IRRef k2 = cando_ir_const(&t, cando_string_value(s2));
    IRRef k3 = cando_ir_const(&t, cando_string_value(s3));

    EXPECT_EQ(k1, k2);
    EXPECT_NEQ(k1, k3);
    EXPECT_EQ(t.const_count, 2u);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_emit_knum_helper) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    IRRef a = cando_ir_emit_knum(&t, 42.0);
    IRRef b = cando_ir_emit_knum(&t, 42.0);  /* same value, dedup pool */

    /* Each call emits its own IR_KNUM instruction even though the
     * underlying constant is reused. */
    EXPECT_NEQ(a, b);
    EXPECT_EQ(t.const_count, 1u);

    const IRIns *ia = cando_ir_get_ins(&t, a);
    const IRIns *ib = cando_ir_get_ins(&t, b);
    EXPECT_EQ((IROp)ia->op, IR_KNUM);
    EXPECT_EQ((IROp)ib->op, IR_KNUM);
    EXPECT_EQ(ia->op1, ib->op1);             /* same const-pool ref */

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_ref_macros) {
    EXPECT_FALSE(IRREF_IS_K(IRREF_NIL));
    EXPECT_FALSE(IRREF_IS_K(7u));
    EXPECT_TRUE(IRREF_IS_K(IRREF_K(7u)));
    EXPECT_EQ(IRREF_KIDX(IRREF_K(7u)), 7u);
    EXPECT_EQ(IRREF_KIDX(IRREF_K(0u)), 0u);
}

TEST(test_ir_get_ins_bounds) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    EXPECT_TRUE(cando_ir_get_ins(&t, 9999u) == NULL);
    EXPECT_TRUE(cando_ir_get_ins(&t, IRREF_K(0u)) == NULL);
    EXPECT_TRUE(cando_ir_get_ins(NULL, 1u) == NULL);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_reset_keeps_buffers) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    cando_ir_emit_knum(&t, 1.0);
    cando_ir_emit_knum(&t, 2.0);
    EXPECT_TRUE(t.ir_count > 1);
    EXPECT_TRUE(t.const_count > 0);

    u32 cap_before = t.ir_cap;
    cando_trace_ir_reset(&t);
    EXPECT_EQ(t.ir_count, 1u);              /* sentinel only */
    EXPECT_EQ((IROp)t.ir[0].op, IR_NOP);
    EXPECT_EQ(t.const_count, 0u);
    EXPECT_EQ(t.ir_cap, cap_before);        /* buffer reused */

    /* Reuse works. */
    IRRef r = cando_ir_emit_knum(&t, 99.0);
    EXPECT_EQ(r, 1u);

    cando_trace_ir_destroy(&t);
}

TEST(test_ir_op_and_type_names) {
    EXPECT_STR(cando_ir_op_name(IR_NOP),  "IR_NOP");
    EXPECT_STR(cando_ir_op_name(IR_ADD),  "IR_ADD");
    EXPECT_STR(cando_ir_op_name(IR_LOOP), "IR_LOOP");
    EXPECT_STR(cando_ir_op_name((IROp)9999), "IR_???");

    EXPECT_STR(cando_ir_type_name(IRT_NUM),  "num");
    EXPECT_STR(cando_ir_type_name(IRT_OBJ),  "obj");
    EXPECT_STR(cando_ir_type_name(IRT_VOID), "void");
}

TEST(test_ir_dump_produces_output) {
    CandoTraceIR t;
    cando_trace_ir_init(&t);

    IRRef a = cando_ir_emit_knum(&t, 1.0);
    IRRef b = cando_ir_emit_knum(&t, 2.0);
    cando_ir_emit(&t, IR_ADD, IRT_NUM, 0, a, b);

    char buf[1024] = {0};
    FILE *f = fmemopen(buf, sizeof(buf) - 1, "w");
    EXPECT_TRUE(f != NULL);
    cando_ir_dump(&t, f);
    fflush(f);
    fclose(f);

    /* Must include the IR_ADD line and at least one constant. */
    EXPECT_TRUE(strstr(buf, "IR_ADD")  != NULL);
    EXPECT_TRUE(strstr(buf, "IR_KNUM") != NULL);

    cando_trace_ir_destroy(&t);
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("\n=== test_jit_ir ===\n");

    run_test("init / destroy",                test_ir_init_destroy);
    run_test("emit basic",                    test_ir_emit_basic);
    run_test("capacity growth",               test_ir_capacity_growth);
    run_test("const pool number dedup",       test_ir_const_pool_number_dedup);
    run_test("const pool string dedup",       test_ir_const_pool_string_dedup);
    run_test("emit_knum helper",              test_ir_emit_knum_helper);
    run_test("IRRef macros",                  test_ir_ref_macros);
    run_test("get_ins bounds",                test_ir_get_ins_bounds);
    run_test("reset keeps buffers",           test_ir_reset_keeps_buffers);
    run_test("op / type names",               test_ir_op_and_type_names);
    run_test("dump produces output",          test_ir_dump_produces_output);

    printf("\n=== Results: %d/%d passed ===\n", g_pass, g_run);
    return g_fail > 0 ? 1 : 0;
}
