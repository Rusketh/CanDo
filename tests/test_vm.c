/*
 * tests/test_vm.c -- Unit tests for the Cando VM: opcodes, chunks, and
 *                   the execution loop.
 *
 * Compile via Makefile:
 *   make test
 *
 * Or manually:
 *   gcc -std=c11 -pthread -D_GNU_SOURCE -lm \
 *       -I source/core -I source/vm \
 *       source/core/common.c source/core/value.c source/core/lock.c \
 *       source/core/handle.c source/core/memory.c \
 *       source/vm/opcodes.c source/vm/chunk.c source/vm/vm.c \
 *       source/vm/debug.c \
 *       tests/test_vm.c -o tests/test_vm && ./tests/test_vm
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "value.h"
#include "opcodes.h"
#include "chunk.h"
#include "vm.h"
#include "debug.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Minimal test harness (same style as test_core.c)
 * ===================================================================== */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_tests_run++; \
        if (cond) { \
            g_tests_passed++; \
        } else { \
            g_tests_failed++; \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define EXPECT_EQ(a, b)   EXPECT((a) == (b))
#define EXPECT_NEQ(a, b)  EXPECT((a) != (b))
#define EXPECT_TRUE(x)    EXPECT(!!(x))
#define EXPECT_FALSE(x)   EXPECT(!(x))
#define EXPECT_STREQ(a,b) EXPECT(strcmp((a),(b)) == 0)

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-50s", name);
    int before = g_tests_failed;
    fn();
    printf("%s\n", g_tests_failed == before ? "OK" : "FAIL");
}

/* =========================================================================
 * Test: opcode names
 * ===================================================================== */
TEST(test_opcode_names) {
    EXPECT_STREQ(cando_opcode_name(OP_ADD),    "OP_ADD");
    EXPECT_STREQ(cando_opcode_name(OP_RETURN), "OP_RETURN");
    EXPECT_STREQ(cando_opcode_name(OP_HALT),   "OP_HALT");
    EXPECT_STREQ(cando_opcode_name(OP_NOP),    "OP_NOP");
    /* Out-of-range returns a safe string. */
    const char *bad = cando_opcode_name((CandoOpcode)OP_COUNT);
    EXPECT_TRUE(bad != NULL);
}

/* =========================================================================
 * Test: opcode formats
 * ===================================================================== */
TEST(test_opcode_formats) {
    EXPECT_EQ(cando_opcode_fmt(OP_ADD),         OPFMT_NONE);
    EXPECT_EQ(cando_opcode_fmt(OP_HALT),        OPFMT_NONE);
    EXPECT_EQ(cando_opcode_fmt(OP_CONST),       OPFMT_A);
    EXPECT_EQ(cando_opcode_fmt(OP_RETURN),      OPFMT_A);
    EXPECT_EQ(cando_opcode_fmt(OP_METHOD_CALL), OPFMT_A_B);
    EXPECT_EQ(cando_opcode_fmt(OP_FLUENT_CALL), OPFMT_A_B);

    /* Sizes derived from format. */
    EXPECT_EQ(cando_opcode_size(OP_ADD),         1u);
    EXPECT_EQ(cando_opcode_size(OP_CONST),       3u);
    EXPECT_EQ(cando_opcode_size(OP_METHOD_CALL), 5u);
}

/* =========================================================================
 * Test: u16 little-endian read/write
 * ===================================================================== */
TEST(test_u16_encoding) {
    u8 buf[2];
    cando_write_u16(buf, 0xABCD);
    EXPECT_EQ(buf[0], 0xCD);
    EXPECT_EQ(buf[1], 0xAB);
    EXPECT_EQ(cando_read_u16(buf), 0xABCD);

    /* Signed roundtrip for negative jump offsets. */
    i16 neg = -100;
    cando_write_u16(buf, (u16)neg);
    EXPECT_EQ(cando_read_i16(buf), -100);
}

/* =========================================================================
 * Test: chunk construction
 * ===================================================================== */
TEST(test_chunk_new_free) {
    CandoChunk *c = cando_chunk_new("test", 0, false);
    EXPECT_TRUE(c != NULL);
    EXPECT_EQ(c->code_len,    0u);
    EXPECT_EQ(c->const_count, 0u);
    EXPECT_EQ(c->arity,       0u);
    EXPECT_FALSE(c->has_vararg);
    cando_chunk_free(c);
}

TEST(test_chunk_emit_bytes) {
    CandoChunk *c = cando_chunk_new("emit_test", 0, false);

    cando_chunk_emit_op(c, OP_NULL, 1);
    cando_chunk_emit_op(c, OP_TRUE, 1);
    cando_chunk_emit_op(c, OP_HALT, 2);

    EXPECT_EQ(c->code_len, 3u);
    EXPECT_EQ(c->code[0],  (u8)OP_NULL);
    EXPECT_EQ(c->code[1],  (u8)OP_TRUE);
    EXPECT_EQ(c->code[2],  (u8)OP_HALT);
    EXPECT_EQ(c->lines[0], 1u);
    EXPECT_EQ(c->lines[2], 2u);

    cando_chunk_free(c);
}

TEST(test_chunk_emit_op_a) {
    CandoChunk *c = cando_chunk_new("op_a", 0, false);

    cando_chunk_emit_op_a(c, OP_LOAD_LOCAL, 42, 1);
    EXPECT_EQ(c->code_len, 3u);
    EXPECT_EQ(c->code[0],  (u8)OP_LOAD_LOCAL);
    EXPECT_EQ(cando_read_u16(&c->code[1]), 42u);

    cando_chunk_free(c);
}

TEST(test_chunk_emit_op_ab) {
    CandoChunk *c = cando_chunk_new("op_ab", 0, false);

    cando_chunk_emit_op_ab(c, OP_METHOD_CALL, 10, 3, 5);
    EXPECT_EQ(c->code_len, 5u);
    EXPECT_EQ(c->code[0],  (u8)OP_METHOD_CALL);
    EXPECT_EQ(cando_read_u16(&c->code[1]), 10u);
    EXPECT_EQ(cando_read_u16(&c->code[3]),  3u);

    cando_chunk_free(c);
}

TEST(test_chunk_const_pool) {
    CandoChunk *c = cando_chunk_new("consts", 0, false);

    u16 i1 = cando_chunk_add_const(c, cando_number(3.14));
    u16 i2 = cando_chunk_add_const(c, cando_number(3.14)); /* dedup */
    u16 i3 = cando_chunk_add_const(c, cando_number(2.71));

    EXPECT_EQ(i1, i2);           /* deduplicated */
    EXPECT_NEQ(i1, i3);
    EXPECT_EQ(c->const_count, 2u);

    EXPECT_TRUE(cando_is_number(c->constants[i1]));
    EXPECT_TRUE(fabs(c->constants[i1].as.number - 3.14) < 1e-9);

    cando_chunk_free(c);
}

TEST(test_chunk_string_const) {
    CandoChunk *c = cando_chunk_new("strings", 0, false);

    u16 a = cando_chunk_add_string_const(c, "hello", 5);
    u16 b = cando_chunk_add_string_const(c, "hello", 5); /* dedup */
    u16 d = cando_chunk_add_string_const(c, "world", 5);

    EXPECT_EQ(a, b);
    EXPECT_NEQ(a, d);
    EXPECT_EQ(c->const_count, 2u);

    cando_chunk_free(c);
}

TEST(test_chunk_jump_patching) {
    CandoChunk *c = cando_chunk_new("jumps", 0, false);

    u32 patch = cando_chunk_emit_jump(c, OP_JUMP_IF_FALSE, 1);
    cando_chunk_emit_op(c, OP_NULL, 1);   /* 1 byte body */
    cando_chunk_patch_jump(c, patch);

    /* patch is at offset 1 (byte after opcode).
     * instruction occupies bytes 0,1,2.  instr_end = patch+2 = 3.
     * code_len after body = 4.  offset = 4 - 3 = 1.                    */
    i16 stored = cando_read_i16(&c->code[patch]);
    EXPECT_EQ(stored, 1);

    cando_chunk_free(c);
}

TEST(test_chunk_loop_emit) {
    CandoChunk *c = cando_chunk_new("loop", 0, false);

    u32 loop_start = c->code_len;
    cando_chunk_emit_op(c, OP_NULL, 1);  /* 1 byte body */
    cando_chunk_emit_loop(c, loop_start, 1);
    /* OP_LOOP at offset 1, operand = (1+3) - 0 = 4 */
    EXPECT_EQ(c->code[1], (u8)OP_LOOP);
    u16 back = cando_read_u16(&c->code[2]);
    EXPECT_EQ(back, 4u);

    cando_chunk_free(c);
}

/* =========================================================================
 * Test: VM lifecycle and stack helpers
 * ===================================================================== */
TEST(test_vm_init_destroy) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    EXPECT_EQ(cando_vm_stack_depth(&vm), 0u);
    EXPECT_EQ(vm.frame_count, 0u);
    EXPECT_EQ(vm.try_depth,   0u);
    EXPECT_EQ(vm.loop_depth,  0u);
    cando_vm_destroy(&vm);
}

TEST(test_vm_push_pop_peek) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);

    cando_vm_push(&vm, cando_number(1.0));
    cando_vm_push(&vm, cando_number(2.0));
    cando_vm_push(&vm, cando_number(3.0));

    EXPECT_EQ(cando_vm_stack_depth(&vm), 3u);

    CandoValue top = cando_vm_peek(&vm, 0);
    EXPECT_TRUE(cando_is_number(top));
    EXPECT_TRUE(fabs(top.as.number - 3.0) < 1e-9);

    CandoValue popped = cando_vm_pop(&vm);
    EXPECT_TRUE(fabs(popped.as.number - 3.0) < 1e-9);
    EXPECT_EQ(cando_vm_stack_depth(&vm), 2u);

    cando_vm_destroy(&vm);
}

/* =========================================================================
 * Test: global variable API
 * ===================================================================== */
TEST(test_vm_globals) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);

    /* Set a mutable global. */
    EXPECT_TRUE(cando_vm_set_global(&vm, "x", cando_number(42.0), false));
    CandoValue out;
    EXPECT_TRUE(cando_vm_get_global(&vm, "x", &out));
    EXPECT_TRUE(fabs(out.as.number - 42.0) < 1e-9);

    /* Overwrite it. */
    EXPECT_TRUE(cando_vm_set_global(&vm, "x", cando_number(99.0), false));
    EXPECT_TRUE(cando_vm_get_global(&vm, "x", &out));
    EXPECT_TRUE(fabs(out.as.number - 99.0) < 1e-9);

    /* Const global cannot be overwritten. */
    EXPECT_TRUE(cando_vm_set_global(&vm, "C", cando_bool(true), true));
    EXPECT_FALSE(cando_vm_set_global(&vm, "C", cando_bool(false), false));
    EXPECT_TRUE(cando_vm_get_global(&vm, "C", &out));
    EXPECT_TRUE(out.as.boolean); /* still true */

    /* Missing global returns false. */
    EXPECT_FALSE(cando_vm_get_global(&vm, "no_such_var", &out));

    cando_vm_destroy(&vm);
}

/* =========================================================================
 * Test: VM execution of trivial programs
 * ===================================================================== */

/* Helper: build and exec a chunk; return result status and top-of-stack
 * number (if any). */
static CandoVMResult exec_simple(const char *name,
                                  void (*build)(CandoChunk *c),
                                  f64 *result_num, bool *result_bool,
                                  CandoVM *out_vm) {
    CandoVM  vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new(name, 0, false);
    build(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    if (result_num)  *result_num  = 0.0;
    if (result_bool) *result_bool = false;
    if (cando_vm_stack_depth(&vm) > 0) {
        CandoValue top = cando_vm_peek(&vm, 0);
        if (result_num  && cando_is_number(top)) *result_num  = top.as.number;
        if (result_bool && cando_is_bool(top))   *result_bool = top.as.boolean;
    }
    if (out_vm) *out_vm = vm;          /* copy state for inspection */
    else        cando_vm_destroy(&vm);
    cando_chunk_free(c);
    return r;
}

/* Chunk: push 1.0, halt — should leave 1.0 on stack. */
static void build_push_number(CandoChunk *c) {
    u16 idx = cando_chunk_add_const(c, cando_number(1.0));
    cando_chunk_emit_op_a(c, OP_CONST, idx, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_push_number) {
    f64 result = 0.0;
    CandoVMResult r = exec_simple("push_num", build_push_number, &result, NULL, NULL);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(fabs(result - 1.0) < 1e-9);
}

/* Chunk: push 3, push 4, ADD, HALT — should leave 7 on stack. */
static void build_add(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(3.0));
    u16 b = cando_chunk_add_const(c, cando_number(4.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_ADD, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_add) {
    f64 result = 0.0;
    CandoVMResult r = exec_simple("add", build_add, &result, NULL, NULL);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(fabs(result - 7.0) < 1e-9);
}

/* Chunk: push 10, push 3, SUB → 7. */
static void build_sub(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(10.0));
    u16 b = cando_chunk_add_const(c, cando_number(3.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_SUB, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_sub) {
    f64 result;
    exec_simple("sub", build_sub, &result, NULL, NULL);
    EXPECT_TRUE(fabs(result - 7.0) < 1e-9);
}

/* Chunk: push 6, push 7, MUL → 42. */
static void build_mul(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(6.0));
    u16 b = cando_chunk_add_const(c, cando_number(7.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_MUL, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_mul) {
    f64 result;
    exec_simple("mul", build_mul, &result, NULL, NULL);
    EXPECT_TRUE(fabs(result - 42.0) < 1e-9);
}

/* Chunk: push 2, push 3, POW → 8. */
static void build_pow(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(2.0));
    u16 b = cando_chunk_add_const(c, cando_number(3.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_POW, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_pow) {
    f64 result;
    exec_simple("pow", build_pow, &result, NULL, NULL);
    EXPECT_TRUE(fabs(result - 8.0) < 1e-9);
}

/* Chunk: push NULL, NOT → true. */
static void build_not_null(CandoChunk *c) {
    cando_chunk_emit_op(c, OP_NULL, 1);
    cando_chunk_emit_op(c, OP_NOT, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_not) {
    bool result = false;
    CandoVMResult r = exec_simple("not_null", build_not_null, NULL, &result, NULL);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(result);
}

/* Chunk: push 5, NEG → -5. */
static void build_neg(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(5.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op(c, OP_NEG, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_neg) {
    f64 result;
    exec_simple("neg", build_neg, &result, NULL, NULL);
    EXPECT_TRUE(fabs(result + 5.0) < 1e-9);
}

/* Chunk: push 3, push 3, EQ → true. */
static void build_eq_true(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(3.0));
    u16 b = cando_chunk_add_const(c, cando_number(3.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_EQ, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_eq_true) {
    bool result = false;
    exec_simple("eq_true", build_eq_true, NULL, &result, NULL);
    EXPECT_TRUE(result);
}

/* Chunk: push 3, push 4, EQ → false. */
static void build_eq_false(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(3.0));
    u16 b = cando_chunk_add_const(c, cando_number(4.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_EQ, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_eq_false) {
    bool result = true; /* init to opposite */
    exec_simple("eq_false", build_eq_false, NULL, &result, NULL);
    EXPECT_FALSE(result);
}

/* Chunk: JUMP forward 1 byte, OP_NULL (skipped), OP_TRUE, HALT → true. */
static void build_jump(CandoChunk *c) {
    /* emit JUMP over the OP_NULL */
    u32 patch = cando_chunk_emit_jump(c, OP_JUMP, 1);
    cando_chunk_emit_op(c, OP_NULL, 1);  /* should be skipped */
    cando_chunk_patch_jump(c, patch);
    cando_chunk_emit_op(c, OP_TRUE, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_jump) {
    bool result = false;
    CandoVMResult r = exec_simple("jump", build_jump, NULL, &result, NULL);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(result);
}

/* Chunk: push FALSE, JUMP_IF_FALSE (peek) to false-path, true-path: POP+NULL+JUMP,
 * false-path: POP, HALT → condition popped on both branches, depth = 1. */
static void build_cond_jump(CandoChunk *c) {
    cando_chunk_emit_op(c, OP_FALSE, 1);
    u32 then_jump = cando_chunk_emit_jump(c, OP_JUMP_IF_FALSE, 1);
    /* true path (not taken): pop condition, push something, jump to end */
    cando_chunk_emit_op(c, OP_POP, 1);
    cando_chunk_emit_op(c, OP_NULL, 1);
    u32 else_jump = cando_chunk_emit_jump(c, OP_JUMP, 1);
    /* false path (taken): pop condition */
    cando_chunk_patch_jump(c, then_jump);
    cando_chunk_emit_op(c, OP_POP, 1);
    cando_chunk_patch_jump(c, else_jump);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_cond_jump) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("cond_jump", 0, false);
    build_cond_jump(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* JUMP_IF_FALSE peeks (does not consume) — the explicit POP on the
     * false branch consumes it.  Only the sentinel remains.            */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 1u);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* Chunk: push TRUE, JUMP_IF_TRUE (peek) to true-path, false-path: POP+JUMP,
 * true-path: POP+NULL, end: HALT → sentinel + NULL on stack, depth = 2. */
static void build_jump_if_true(CandoChunk *c) {
    cando_chunk_emit_op(c, OP_TRUE, 1);
    u32 then_jump = cando_chunk_emit_jump(c, OP_JUMP_IF_TRUE, 1);
    /* false path (not taken): pop condition, jump to end */
    cando_chunk_emit_op(c, OP_POP, 1);
    u32 else_jump = cando_chunk_emit_jump(c, OP_JUMP, 1);
    /* true path (taken): pop condition, push NULL */
    cando_chunk_patch_jump(c, then_jump);
    cando_chunk_emit_op(c, OP_POP, 1);
    cando_chunk_emit_op(c, OP_NULL, 1);
    cando_chunk_patch_jump(c, else_jump);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_jump_if_true) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("jit", 0, false);
    build_jump_if_true(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* JUMP_IF_TRUE peeks — explicit POP on true branch consumes condition,
     * then NULL is pushed.  sentinel + NULL = depth 2.                  */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 2u);
    EXPECT_TRUE(cando_is_null(cando_vm_peek(&vm, 0)));
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* Chunk: push 2, INCR → 3. */
static void build_incr(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(2.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op(c, OP_INCR, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_incr_decr) {
    f64 result;
    exec_simple("incr", build_incr, &result, NULL, NULL);
    EXPECT_TRUE(fabs(result - 3.0) < 1e-9);
}

/* Chunk: RETURN 0 values from top-level → VM_HALT. */
static void build_return0(CandoChunk *c) {
    cando_chunk_emit_op_a(c, OP_RETURN, 0, 1);
}

TEST(test_exec_return_zero) {
    CandoVMResult r = exec_simple("ret0", build_return0, NULL, NULL, NULL);
    EXPECT_EQ(r, VM_HALT);
}

/* Chunk: RETURN 1 value (a number). */
static void build_return_num(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(99.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_RETURN, 1, 1);
}

TEST(test_exec_return_num) {
    f64 result = 0.0;
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("ret_num", 0, false);
    build_return_num(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* At top level, RETURN pops the frame; the return value ends up
     * on the stack before the frame pop.  Top-level return clears the
     * stack; nothing left for inspection.  Just check success.         */
    CANDO_UNUSED(result);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* Chunk: bitwise operations. */
static void build_bitwise(CandoChunk *c) {
    /* 0xA & 0xC = 0x8 = 8 */
    u16 a = cando_chunk_add_const(c, cando_number(0xA));
    u16 b = cando_chunk_add_const(c, cando_number(0xC));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_BIT_AND, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_bitwise) {
    f64 result;
    exec_simple("bitwise", build_bitwise, &result, NULL, NULL);
    EXPECT_EQ((i64)result, 8);
}

/* Chunk: range ascending 1->4, push 4 values onto stack. */
static void build_range_asc(CandoChunk *c) {
    u16 a = cando_chunk_add_const(c, cando_number(1.0));
    u16 b = cando_chunk_add_const(c, cando_number(4.0));
    cando_chunk_emit_op_a(c, OP_CONST, a, 1);
    cando_chunk_emit_op_a(c, OP_CONST, b, 1);
    cando_chunk_emit_op(c, OP_RANGE_ASC, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_range_asc) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("range_asc", 0, false);
    build_range_asc(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* sentinel at bottom + 4 range values + count: total depth 6 */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 6u);
    /* peek(0)=count(4), peek(1)=4, peek(2)=3, peek(3)=2, peek(4)=1, peek(5)=sentinel */
    EXPECT_TRUE(fabs(cando_vm_peek(&vm, 0).as.number - 4.0) < 1e-9); /* count */
    EXPECT_TRUE(fabs(cando_vm_peek(&vm, 4).as.number - 1.0) < 1e-9); /* first val */
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* Chunk: try block with throw + catch (single value, no binding). */
static void build_try_catch(CandoChunk *c) {
    /*
     * TRY_BEGIN  → catch_block
     *   CONST "oops"
     *   THROW 1          ← OP_THROW now takes a count operand
     * TRY_END
     * JUMP → end         ← skips catch on normal exit
     * catch_block:
     *   CATCH_BEGIN 0    ← 0 params: nothing pushed, exception discarded
     *   TRUE             ← signal that catch ran
     * end:
     * HALT
     */
    u32 try_patch = cando_chunk_emit_jump(c, OP_TRY_BEGIN, 1);

    /* throw body */
    u16 err_idx = cando_chunk_add_string_const(c, "oops", 4);
    cando_chunk_emit_op_a(c, OP_CONST, err_idx, 1);
    cando_chunk_emit_op_a(c, OP_THROW, 1, 1);   /* count = 1 */

    cando_chunk_emit_op(c, OP_TRY_END, 1);
    u32 skip_patch = cando_chunk_emit_jump(c, OP_JUMP, 1);

    /* patch TRY_BEGIN to jump here */
    cando_chunk_patch_jump(c, try_patch);

    /* CATCH_BEGIN 0: no params bound, nothing pushed onto stack */
    cando_chunk_emit_op_a(c, OP_CATCH_BEGIN, 0, 1);
    cando_chunk_emit_op(c, OP_TRUE, 1);   /* signal catch ran */

    /* patch JUMP after TRY_END to skip the catch on normal exit */
    cando_chunk_patch_jump(c, skip_patch);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_try_catch) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("try_catch", 0, false);
    build_try_catch(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* After catch the stack should have TRUE on top. */
    if (cando_vm_stack_depth(&vm) > 0) {
        CandoValue top = cando_vm_peek(&vm, 0);
        EXPECT_TRUE(cando_is_bool(top) && top.as.boolean);
    } else {
        EXPECT_TRUE(false); /* expected TRUE on stack */
    }
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: multi-arg throw — throw 2 values, catch both.
 *
 * THROW 42, 99  →  CATCH (a, b)
 * CATCH_BEGIN 2 pushes [99, 42] (reversed so top = first arg).
 * After HALT: peek(0) = 42, peek(1) = 99.
 * ===================================================================== */
static void build_throw_multi(CandoChunk *c) {
    u32 try_patch = cando_chunk_emit_jump(c, OP_TRY_BEGIN, 1);

    u16 v42 = cando_chunk_add_const(c, cando_number(42.0));
    u16 v99 = cando_chunk_add_const(c, cando_number(99.0));
    cando_chunk_emit_op_a(c, OP_CONST, v42, 1);   /* first arg  */
    cando_chunk_emit_op_a(c, OP_CONST, v99, 1);   /* second arg */
    cando_chunk_emit_op_a(c, OP_THROW, 2, 1);     /* throw 2    */

    cando_chunk_emit_op(c, OP_TRY_END, 1);
    u32 skip_patch = cando_chunk_emit_jump(c, OP_JUMP, 1);

    cando_chunk_patch_jump(c, try_patch);
    /* CATCH_BEGIN 2: pushes error_vals[1] then error_vals[0].
     * Stack top = first thrown arg (42).                          */
    cando_chunk_emit_op_a(c, OP_CATCH_BEGIN, 2, 1);

    cando_chunk_patch_jump(c, skip_patch);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_throw_multi) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("throw_multi", 0, false);
    build_throw_multi(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(cando_vm_stack_depth(&vm) >= 2u);
    CandoValue first  = cando_vm_peek(&vm, 0); /* top = first thrown arg  */
    CandoValue second = cando_vm_peek(&vm, 1); /* next = second thrown arg */
    EXPECT_TRUE(cando_is_number(first)
                && fabs(first.as.number  - 42.0) < 1e-9);
    EXPECT_TRUE(cando_is_number(second)
                && fabs(second.as.number - 99.0) < 1e-9);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: throw fewer args than catch declares — extras become null.
 *
 * THROW 42  →  CATCH (a, b)
 * CATCH_BEGIN 2: stack top = 42, next = null.
 * ===================================================================== */
static void build_throw_fewer(CandoChunk *c) {
    u32 try_patch = cando_chunk_emit_jump(c, OP_TRY_BEGIN, 1);

    u16 v42 = cando_chunk_add_const(c, cando_number(42.0));
    cando_chunk_emit_op_a(c, OP_CONST, v42, 1);
    cando_chunk_emit_op_a(c, OP_THROW, 1, 1);   /* throw 1 value */

    cando_chunk_emit_op(c, OP_TRY_END, 1);
    u32 skip_patch = cando_chunk_emit_jump(c, OP_JUMP, 1);

    cando_chunk_patch_jump(c, try_patch);
    /* CATCH_BEGIN 2 with only 1 thrown value: pushes null then 42.
     * Stack top = 42, peek(1) = null.                             */
    cando_chunk_emit_op_a(c, OP_CATCH_BEGIN, 2, 1);

    cando_chunk_patch_jump(c, skip_patch);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_throw_fewer_than_catch) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("throw_fewer", 0, false);
    build_throw_fewer(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(cando_vm_stack_depth(&vm) >= 2u);
    CandoValue first  = cando_vm_peek(&vm, 0); /* thrown value */
    CandoValue second = cando_vm_peek(&vm, 1); /* padded null  */
    EXPECT_TRUE(cando_is_number(first)
                && fabs(first.as.number - 42.0) < 1e-9);
    EXPECT_TRUE(cando_is_null(second));
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: throw more args than catch declares — extras are silently dropped.
 *
 * THROW 42, 99  →  CATCH (a)   — only first arg bound.
 * CATCH_BEGIN 1: stack top = 42 only.
 * ===================================================================== */
static void build_throw_more(CandoChunk *c) {
    u32 try_patch = cando_chunk_emit_jump(c, OP_TRY_BEGIN, 1);

    u16 v42 = cando_chunk_add_const(c, cando_number(42.0));
    u16 v99 = cando_chunk_add_const(c, cando_number(99.0));
    cando_chunk_emit_op_a(c, OP_CONST, v42, 1);
    cando_chunk_emit_op_a(c, OP_CONST, v99, 1);
    cando_chunk_emit_op_a(c, OP_THROW, 2, 1);   /* throw 2 values */

    cando_chunk_emit_op(c, OP_TRY_END, 1);
    u32 skip_patch = cando_chunk_emit_jump(c, OP_JUMP, 1);

    cando_chunk_patch_jump(c, try_patch);
    /* CATCH_BEGIN 1: only binds first thrown arg; second is dropped. */
    cando_chunk_emit_op_a(c, OP_CATCH_BEGIN, 1, 1);

    cando_chunk_patch_jump(c, skip_patch);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_throw_more_than_catch) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("throw_more", 0, false);
    build_throw_more(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(cando_vm_stack_depth(&vm) >= 1u);
    CandoValue first = cando_vm_peek(&vm, 0);
    EXPECT_TRUE(cando_is_number(first)
                && fabs(first.as.number - 42.0) < 1e-9);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: runtime error (vm_runtime_error) is catchable via TRY/CATCH.
 *
 * A native that calls cando_vm_error() sets error_vals[0] to the message
 * string. CATCH_BEGIN 1 should expose it.
 * ===================================================================== */
static int native_always_error(CandoVM *vm, int argc, CandoValue *args) {
    (void)argc; (void)args;
    cando_vm_error(vm, "test error from native");
    return -1;
}

static void build_catch_native_error(CandoChunk *c) {
    /* We cannot easily call a native from raw bytecode without registering
     * it first — so we test via OP_THROW 1 (string) instead, which mirrors
     * exactly what cando_vm_error sets in error_vals[0].               */
    u32 try_patch = cando_chunk_emit_jump(c, OP_TRY_BEGIN, 1);

    u16 msg = cando_chunk_add_string_const(c, "test error from native", 22);
    cando_chunk_emit_op_a(c, OP_CONST, msg, 1);
    cando_chunk_emit_op_a(c, OP_THROW, 1, 1);

    cando_chunk_emit_op(c, OP_TRY_END, 1);
    u32 skip_patch = cando_chunk_emit_jump(c, OP_JUMP, 1);

    cando_chunk_patch_jump(c, try_patch);
    cando_chunk_emit_op_a(c, OP_CATCH_BEGIN, 1, 1);  /* bind the string */

    cando_chunk_patch_jump(c, skip_patch);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_catch_native_error) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    cando_vm_register_native(&vm, "_always_error", native_always_error);

    CandoChunk *c = cando_chunk_new("catch_native_error", 0, false);
    build_catch_native_error(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(cando_vm_stack_depth(&vm) >= 1u);
    CandoValue caught = cando_vm_peek(&vm, 0);
    EXPECT_TRUE(cando_is_string(caught));
    if (cando_is_string(caught)) {
        EXPECT_TRUE(strncmp(caught.as.string->data,
                            "test error from native", 22) == 0);
    }
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* Chunk: NOP + HALT. */
static void build_nop(CandoChunk *c) {
    cando_chunk_emit_op(c, OP_NOP, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_nop) {
    CandoVMResult r = exec_simple("nop", build_nop, NULL, NULL, NULL);
    EXPECT_EQ(r, VM_HALT);
}

/* Chunk: multi-value EQ_STACK — 5 == 5, 5 → true (AND: all rights equal left). */
static void build_eq_stack(CandoChunk *c) {
    u16 v5a = cando_chunk_add_const(c, cando_number(5.0));
    u16 v5b = cando_chunk_add_const(c, cando_number(5.0));
    u16 v5c = cando_chunk_add_const(c, cando_number(5.0));

    /* Push left operand, then 2 right values (both 5). */
    cando_chunk_emit_op_a(c, OP_CONST, v5a, 1); /* left = 5 */
    cando_chunk_emit_op_a(c, OP_CONST, v5b, 1); /* right[0] = 5 */
    cando_chunk_emit_op_a(c, OP_CONST, v5c, 1); /* right[1] = 5 */
    cando_chunk_emit_op_a(c, OP_EQ_STACK, 2, 1); /* 5 == 5 && 5 == 5 → true */
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_eq_stack) {
    bool result = false;
    CandoVMResult r = exec_simple("eq_stack", build_eq_stack, NULL, &result, NULL);
    EXPECT_EQ(r, VM_HALT);
    EXPECT_TRUE(result);
}

/* =========================================================================
 * Multi-comparison *_STACK opcode tests.
 * Stack convention: [..., left, right0, right1, ..., rightN-1]
 * Opcode pops N rights then left, pushes bool.
 * EQ_STACK  : true if left == ALL rights  (chained equality)
 * NEQ_STACK : true if left != ALL rights
 * LT/GT/LEQ/GEQ_STACK : true if relation holds for ALL rights
 * ===================================================================== */

/* Helper: build left OP right0, right1 (n rights) → bool on stack */
#define BUILD_CMP_STACK(fn_name, LEFT, OP_STACK, N, ...)         \
static void fn_name(CandoChunk *c) {                              \
    f64 rights[] = { __VA_ARGS__ };                               \
    u16 lc = cando_chunk_add_const(c, cando_number(LEFT));        \
    cando_chunk_emit_op_a(c, OP_CONST, lc, 1);                    \
    for (int i = 0; i < (N); i++) {                               \
        u16 rc = cando_chunk_add_const(c, cando_number(rights[i]));\
        cando_chunk_emit_op_a(c, OP_CONST, rc, 1);                \
    }                                                             \
    cando_chunk_emit_op_a(c, OP_STACK, (u16)(N), 1);              \
    cando_chunk_emit_op(c, OP_HALT, 1);                           \
}

/* EQ_STACK: 5 == 5,5 → true (above); 5 == 3,4 → false */
BUILD_CMP_STACK(build_eq_stack_false, 5.0, OP_EQ_STACK, 2, 3.0, 4.0)

/* NEQ_STACK: 1 != 2,3 → true; 2 != 1,2 → false */
BUILD_CMP_STACK(build_neq_stack_true,  1.0, OP_NEQ_STACK, 2, 2.0, 3.0)
BUILD_CMP_STACK(build_neq_stack_false, 2.0, OP_NEQ_STACK, 2, 1.0, 2.0)

/* LT_STACK: 1 < 2,3,4 → true; 3 < 1,4 → false */
BUILD_CMP_STACK(build_lt_stack_true,  1.0, OP_LT_STACK, 3, 2.0, 3.0, 4.0)
BUILD_CMP_STACK(build_lt_stack_false, 3.0, OP_LT_STACK, 2, 1.0, 4.0)

/* GT_STACK: 5 > 1,2,3 → true; 1 > 2,3 → false */
BUILD_CMP_STACK(build_gt_stack_true,  5.0, OP_GT_STACK, 3, 1.0, 2.0, 3.0)
BUILD_CMP_STACK(build_gt_stack_false, 1.0, OP_GT_STACK, 2, 2.0, 3.0)

/* LEQ_STACK: 2 <= 2,3 → true; 3 <= 2,4 → false */
BUILD_CMP_STACK(build_leq_stack_true,  2.0, OP_LEQ_STACK, 2, 2.0, 3.0)
BUILD_CMP_STACK(build_leq_stack_false, 3.0, OP_LEQ_STACK, 2, 2.0, 4.0)

/* GEQ_STACK: 3 >= 1,3 → true; 1 >= 2,0 → false */
BUILD_CMP_STACK(build_geq_stack_true,  3.0, OP_GEQ_STACK, 2, 1.0, 3.0)
BUILD_CMP_STACK(build_geq_stack_false, 1.0, OP_GEQ_STACK, 2, 2.0, 0.0)

TEST(test_exec_cmp_stack) {
    bool r; CandoVMResult res;

    /* EQ_STACK false: 5 == 3,4 → false */
    res = exec_simple("eq_stack_false", build_eq_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);

    /* NEQ_STACK true: 1 != 2,3 → true */
    res = exec_simple("neq_stack_true",  build_neq_stack_true,  NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_TRUE(r);

    /* NEQ_STACK false: 2 != 1,2 → false (2 equals 2) */
    res = exec_simple("neq_stack_false", build_neq_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);

    /* LT_STACK true: 1 < 2,3,4 → true */
    res = exec_simple("lt_stack_true",  build_lt_stack_true,  NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_TRUE(r);

    /* LT_STACK false: 3 < 1,4 → false (3 is not < 1) */
    res = exec_simple("lt_stack_false", build_lt_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);

    /* GT_STACK true: 5 > 1,2,3 → true */
    res = exec_simple("gt_stack_true",  build_gt_stack_true,  NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_TRUE(r);

    /* GT_STACK false: 1 > 2,3 → false */
    res = exec_simple("gt_stack_false", build_gt_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);

    /* LEQ_STACK true: 2 <= 2,3 → true */
    res = exec_simple("leq_stack_true",  build_leq_stack_true,  NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_TRUE(r);

    /* LEQ_STACK false: 3 <= 2,4 → false */
    res = exec_simple("leq_stack_false", build_leq_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);

    /* GEQ_STACK true: 3 >= 1,3 → true */
    res = exec_simple("geq_stack_true",  build_geq_stack_true,  NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_TRUE(r);

    /* GEQ_STACK false: 1 >= 2,0 → false (1 is not >= 2) */
    res = exec_simple("geq_stack_false", build_geq_stack_false, NULL, &r, NULL);
    EXPECT_EQ(res, VM_HALT); EXPECT_FALSE(r);
}

/* =========================================================================
 * Test: multi-return — user-defined function returns 2 values
 * =====================================================================
 * Chunk layout:
 *   [0]  OP_JUMP → main         (skip function body)
 *   [3]  fn: OP_CONST(10), OP_CONST(20), OP_RETURN 2
 *   [12] main: OP_CONST(3.0 = fn PC), OP_CALL 0, OP_HALT
 * After call: stack = [NULL-sentinel, 10, 20]
 * ===================================================================== */
static void build_multi_return(CandoChunk *c) {
    /* Jump past function body */
    u32 skip = cando_chunk_emit_jump(c, OP_JUMP, 1);

    /* Function body at offset 3 */
    u16 c10 = cando_chunk_add_const(c, cando_number(10.0));
    u16 c20 = cando_chunk_add_const(c, cando_number(20.0));
    cando_chunk_emit_op_a(c, OP_CONST, c10, 1);
    cando_chunk_emit_op_a(c, OP_CONST, c20, 1);
    cando_chunk_emit_op_a(c, OP_RETURN, 2, 1);

    /* Main code: patch jump, push fn PC (3), call it */
    cando_chunk_patch_jump(c, skip);
    u16 fn_pc = cando_chunk_add_const(c, cando_number(3.0));
    cando_chunk_emit_op_a(c, OP_CONST, fn_pc, 1);
    cando_chunk_emit_op_a(c, OP_CALL, 0, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_multi_return) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("multi_ret", 0, false);
    build_multi_return(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* Stack: NULL-sentinel + 10 + 20 = depth 3 */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 3u);
    EXPECT_TRUE(fabs(cando_vm_peek(&vm, 0).as.number - 20.0) < 1e-9);
    EXPECT_TRUE(fabs(cando_vm_peek(&vm, 1).as.number - 10.0) < 1e-9);
    /* last_ret_count should be 2 after the call */
    EXPECT_EQ(vm.last_ret_count, 2);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: OP_SPREAD_RET — spread inner call's multi-return into outer argc
 * =====================================================================
 * inner_fn (offset 3) returns 5 and 7 (2 values).
 * Chunk after jump:
 *   push outer_native (-1.0), push inner_fn (3.0),
 *   OP_CALL 0, OP_SPREAD_RET, OP_CONST(100), OP_CALL 2, OP_HALT
 * outer_native receives 3 args: [5, 7, 100]
 * We verify spread_extra was consumed (=0) and native got 3 args.
 * ===================================================================== */
static int g_spread_test_argc = -1;
static int spread_test_native(CandoVM *vm, int argc, CandoValue *args) {
    (void)vm; (void)args;
    g_spread_test_argc = argc;
    return 0;
}

static void build_spread_ret(CandoChunk *c) {
    /* Jump past inner function body */
    u32 skip = cando_chunk_emit_jump(c, OP_JUMP, 1);

    /* inner_fn at offset 3: push 5, push 7, return 2 */
    u16 c5 = cando_chunk_add_const(c, cando_number(5.0));
    u16 c7 = cando_chunk_add_const(c, cando_number(7.0));
    cando_chunk_emit_op_a(c, OP_CONST, c5, 1);
    cando_chunk_emit_op_a(c, OP_CONST, c7, 1);
    cando_chunk_emit_op_a(c, OP_RETURN, 2, 1);

    /* Main code */
    cando_chunk_patch_jump(c, skip);
    /* outer_native is at index 0 → sentinel -1.0 */
    u16 outer_idx = cando_chunk_add_const(c, cando_number(-1.0));
    u16 inner_idx = cando_chunk_add_const(c, cando_number(3.0));  /* fn PC */
    u16 c100     = cando_chunk_add_const(c, cando_number(100.0));

    cando_chunk_emit_op_a(c, OP_CONST, outer_idx, 1);  /* outer fn */
    cando_chunk_emit_op_a(c, OP_CONST, inner_idx, 1);  /* inner fn */
    cando_chunk_emit_op_a(c, OP_CALL, 0, 1);            /* call inner: returns 5,7 */
    cando_chunk_emit_op(c, OP_SPREAD_RET, 1);           /* spread_extra += 2-1=1 */
    cando_chunk_emit_op_a(c, OP_CONST, c100, 1);        /* push 100 */
    cando_chunk_emit_op_a(c, OP_CALL, 2, 1);            /* call outer: argc=2+1=3 */
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_spread_ret) {
    g_spread_test_argc = -1;
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    cando_vm_register_native(&vm, "spread_outer", spread_test_native);
    CandoChunk *c = cando_chunk_new("spread_ret", 0, false);
    build_spread_ret(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* outer native received 3 args (5, 7 from spread + 100) */
    EXPECT_EQ(g_spread_test_argc, 3);
    EXPECT_EQ(vm.spread_extra, 0);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: FOR_INIT + FOR_NEXT over a range
 * =====================================================================
 * Iterates over range 1->3, popping each loop variable.
 * After the loop, stack should be empty (just the NULL-sentinel at slot 0).
 *
 * Stack trace:
 *   RANGE_ASC → [1,2,3,count=3]
 *   FOR_INIT  → [1,2,3, count=3, index=0]
 *   FOR_NEXT  → pushes 1, body POPs it; repeat for 2, 3
 *   exit: FOR_NEXT pops [count,index] and [1,2,3] → stack depth = 1
 * ===================================================================== */
static void build_for_range(CandoChunk *c) {
    u16 c1 = cando_chunk_add_const(c, cando_number(1.0));
    u16 c3 = cando_chunk_add_const(c, cando_number(3.0));
    cando_chunk_emit_op_a(c, OP_CONST, c1, 1);
    cando_chunk_emit_op_a(c, OP_CONST, c3, 1);
    cando_chunk_emit_op(c, OP_RANGE_ASC, 1);       /* pushes 1,2,3,count=3 */
    cando_chunk_emit_op_a(c, OP_FOR_INIT, 1, 1);   /* pops count, pushes state */

    u32 loop_start = c->code_len;
    u32 exit_jump  = cando_chunk_emit_jump(c, OP_FOR_NEXT, 1);

    /* Body: pop the x pushed by FOR_NEXT */
    cando_chunk_emit_op(c, OP_POP, 1);

    /* Loop back: offset = (loop_patch+3) - loop_start */
    u32 loop_patch = c->code_len;
    cando_chunk_emit_op_a(c, OP_LOOP, 0, 1);  /* placeholder */
    u16 loop_off = (u16)(c->code_len - loop_start);
    cando_write_u16(&c->code[loop_patch + 1], loop_off);

    cando_chunk_patch_jump(c, exit_jump);
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_for_range) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("for_range", 0, false);
    build_for_range(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* After iterating [1,2,3] with body=POP, state is cleaned up: depth=1 */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 1u);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * Test: disassembler smoke test
 * ===================================================================== */
TEST(test_disasm_smoke) {
    CandoChunk *c = cando_chunk_new("disasm_test", 0, false);
    u16 idx = cando_chunk_add_const(c, cando_number(42.0));
    cando_chunk_emit_op_a(c, OP_CONST, idx, 1);
    cando_chunk_emit_op(c, OP_NEG, 1);
    cando_chunk_emit_op(c, OP_HALT, 1);

    /* Must not crash. */
    cando_chunk_disasm(c, stdout);

    cando_chunk_free(c);
}

/* =========================================================================
 * Test: MASK_SKIP drops top of stack
 * ===================================================================== */
static void build_mask_skip(CandoChunk *c) {
    cando_chunk_emit_op(c, OP_TRUE, 1);
    cando_chunk_emit_op(c, OP_NULL, 1);
    cando_chunk_emit_op(c, OP_MASK_SKIP, 1); /* drops NULL */
    cando_chunk_emit_op(c, OP_HALT, 1);
}

TEST(test_exec_mask_skip) {
    CandoVM vm;
    cando_vm_init(&vm, NULL);
    CandoChunk *c = cando_chunk_new("mask_skip", 0, false);
    build_mask_skip(c);
    CandoVMResult r = cando_vm_exec(&vm, c);
    EXPECT_EQ(r, VM_HALT);
    /* sentinel + TRUE: depth 2; NULL was skipped by MASK_SKIP. */
    EXPECT_EQ(cando_vm_stack_depth(&vm), 2u);
    EXPECT_TRUE(cando_is_bool(cando_vm_peek(&vm, 0)));
    EXPECT_TRUE(cando_vm_peek(&vm, 0).as.boolean);
    cando_vm_destroy(&vm);
    cando_chunk_free(c);
}

/* =========================================================================
 * main
 * ===================================================================== */
int main(void) {
    printf("=== Cando VM Tests ===\n\n");

    printf("Opcode metadata:\n");
    run_test("opcode_names",   test_opcode_names);
    run_test("opcode_formats", test_opcode_formats);
    run_test("u16_encoding",   test_u16_encoding);

    printf("\nChunk construction:\n");
    run_test("chunk_new_free",       test_chunk_new_free);
    run_test("chunk_emit_bytes",     test_chunk_emit_bytes);
    run_test("chunk_emit_op_a",      test_chunk_emit_op_a);
    run_test("chunk_emit_op_ab",     test_chunk_emit_op_ab);
    run_test("chunk_const_pool",     test_chunk_const_pool);
    run_test("chunk_string_const",   test_chunk_string_const);
    run_test("chunk_jump_patching",  test_chunk_jump_patching);
    run_test("chunk_loop_emit",      test_chunk_loop_emit);

    printf("\nVM lifecycle:\n");
    run_test("vm_init_destroy", test_vm_init_destroy);
    run_test("vm_push_pop_peek", test_vm_push_pop_peek);
    run_test("vm_globals",       test_vm_globals);

    printf("\nVM execution:\n");
    run_test("exec_push_number",  test_exec_push_number);
    run_test("exec_add",          test_exec_add);
    run_test("exec_sub",          test_exec_sub);
    run_test("exec_mul",          test_exec_mul);
    run_test("exec_pow",          test_exec_pow);
    run_test("exec_not",          test_exec_not);
    run_test("exec_neg",          test_exec_neg);
    run_test("exec_eq_true",      test_exec_eq_true);
    run_test("exec_eq_false",     test_exec_eq_false);
    run_test("exec_jump",         test_exec_jump);
    run_test("exec_cond_jump",    test_exec_cond_jump);
    run_test("exec_jump_if_true", test_exec_jump_if_true);
    run_test("exec_incr_decr",    test_exec_incr_decr);
    run_test("exec_return_zero",  test_exec_return_zero);
    run_test("exec_return_num",   test_exec_return_num);
    run_test("exec_bitwise",      test_exec_bitwise);
    run_test("exec_range_asc",    test_exec_range_asc);
    run_test("exec_try_catch",             test_exec_try_catch);
    run_test("exec_throw_multi",           test_exec_throw_multi);
    run_test("exec_throw_fewer_than_catch",test_exec_throw_fewer_than_catch);
    run_test("exec_throw_more_than_catch", test_exec_throw_more_than_catch);
    run_test("exec_catch_native_error",    test_exec_catch_native_error);
    run_test("exec_nop",                   test_exec_nop);
    run_test("exec_eq_stack",     test_exec_eq_stack);
    run_test("exec_cmp_stack",    test_exec_cmp_stack);
    run_test("exec_mask_skip",    test_exec_mask_skip);
    run_test("exec_multi_return", test_exec_multi_return);
    run_test("exec_spread_ret",   test_exec_spread_ret);
    run_test("exec_for_range",    test_exec_for_range);

    printf("\nDisassembler:\n");
    run_test("disasm_smoke", test_disasm_smoke);

    printf("\n=== Results: %d/%d passed ===\n",
           g_tests_passed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
