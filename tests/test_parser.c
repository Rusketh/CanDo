/*
 * tests/test_parser.c -- Unit tests for source/parser/parser.h and parser.c
 *
 * Verifies compilation against the unified VM opcode set (CandoOpcode /
 * source/vm/opcodes.h) and the heap-allocated CandoChunk (source/vm/chunk.h).
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "value.h"
#include "parser.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ------------------------------------------------------------------------ */
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

#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))
#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)

static void run_test(const char *name, void (*fn)(void))
{
    printf("  %-55s ", name);
    fflush(stdout);
    int before = g_tests_failed;
    fn();
    printf("%s\n", (g_tests_failed == before) ? "OK" : "FAILED");
}

/* -------------------------------------------------------------------------
 * Helper: compile source and return the result.
 * Caller must call cando_chunk_free(r.chunk) when done.
 * ------------------------------------------------------------------------ */
typedef struct {
    CandoChunk  *chunk;
    CandoParser  parser;
    bool         ok;
} CompileResult;

static CompileResult compile(const char *src)
{
    CompileResult r;
    r.chunk = cando_chunk_new("<test>", 0, false);
    cando_parser_init(&r.parser, src, strlen(src), r.chunk);
    r.ok = cando_parse(&r.parser);
    return r;
}

/* Compile, assert success, return chunk (caller must cando_chunk_free). */
static CandoChunk *compile_ok(const char *src)
{
    CompileResult r = compile(src);
    if (!r.ok) {
        fprintf(stderr, "  [compile_ok failed] %s\n  source: %s\n",
                cando_parser_error(&r.parser), src);
    }
    return r.chunk;
}

/* Find first occurrence of `op` in chunk, advancing by instruction width.
 * Returns byte index or -1 if not found.                                 */
static int find_op(const CandoChunk *c, CandoOpcode op)
{
    u32 i = 0;
    while (i < c->code_len) {
        CandoOpcode cur = (CandoOpcode)c->code[i];
        if (cur == op) return (int)i;
        u32 sz = cando_opcode_size(cur);
        i += (sz > 0) ? sz : 1;
    }
    return -1;
}

/* Count occurrences of `op` advancing by instruction width.              */
static int count_op(const CandoChunk *c, CandoOpcode op)
{
    int n = 0;
    u32 i = 0;
    while (i < c->code_len) {
        CandoOpcode cur = (CandoOpcode)c->code[i];
        if (cur == op) n++;
        u32 sz = cando_opcode_size(cur);
        i += (sz > 0) ? sz : 1;
    }
    return n;
}

/* Check that constant at index idx is a number equal to expected. */
static bool const_is_number(const CandoChunk *c, u32 idx, f64 expected)
{
    if (idx >= c->const_count) return false;
    CandoValue v = c->constants[idx];
    if (!cando_is_number(v)) return false;
    return v.as.number == expected;
}

/* Check that constant at index idx is a string equal to expected. */
static bool const_is_string(const CandoChunk *c, u32 idx, const char *expected)
{
    if (idx >= c->const_count) return false;
    CandoValue v = c->constants[idx];
    if (!cando_is_string(v)) return false;
    CandoString *s = v.as.string;
    return s->length == (u32)strlen(expected) &&
           memcmp(s->data, expected, s->length) == 0;
}

/* -------------------------------------------------------------------------
 * Tests: chunk management
 * ------------------------------------------------------------------------ */
TEST(test_chunk_init_free)
{
    CandoChunk *c = cando_chunk_new("test", 0, false);
    EXPECT_EQ(c->code_len,    0u);
    EXPECT_EQ(c->const_count, 0u);
    cando_chunk_free(c);
}

TEST(test_chunk_write_bytes)
{
    CandoChunk *c = cando_chunk_new("test", 0, false);
    cando_chunk_emit_byte(c, (u8)OP_NULL, 1);
    cando_chunk_emit_byte(c, (u8)OP_POP,  1);
    EXPECT_EQ(c->code_len, 2u);
    EXPECT_EQ(c->code[0], (u8)OP_NULL);
    EXPECT_EQ(c->code[1], (u8)OP_POP);
    cando_chunk_free(c);
}

TEST(test_chunk_add_constant)
{
    CandoChunk *c = cando_chunk_new("test", 0, false);
    u16 idx = cando_chunk_add_const(c, cando_number(3.14));
    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(c->const_count, 1u);
    EXPECT_TRUE(cando_is_number(c->constants[0]));
    cando_chunk_free(c);
}

/* Verify jump patching writes a correct little-endian signed offset.
 * Emit OP_JUMP (placeholder), then OP_NOP, then patch the jump so it
 * lands at code_len (i.e. just past the NOP).  Expected offset = 1.     */
TEST(test_chunk_patch_jump)
{
    CandoChunk *c = cando_chunk_new("test", 0, false);
    u32 patch = cando_chunk_emit_jump(c, OP_JUMP, 1);
    cando_chunk_emit_op(c, OP_NOP, 1);   /* 1 byte past the jump instr */
    cando_chunk_patch_jump(c, patch);
    /* patch points at the lo byte of the u16 operand; offset should be 1 */
    EXPECT_EQ(c->code[patch],     (u8)0x01);  /* low byte  */
    EXPECT_EQ(c->code[patch + 1], (u8)0x00);  /* high byte */
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: opcode names (via VM's cando_opcode_name)
 * ------------------------------------------------------------------------ */
TEST(test_opcode_names)
{
    EXPECT_STR(cando_opcode_name(OP_CONST),          "OP_CONST");
    EXPECT_STR(cando_opcode_name(OP_NULL),           "OP_NULL");
    EXPECT_STR(cando_opcode_name(OP_ADD),            "OP_ADD");
    EXPECT_STR(cando_opcode_name(OP_JUMP),           "OP_JUMP");
    EXPECT_STR(cando_opcode_name(OP_HALT),           "OP_HALT");
    EXPECT_STR(cando_opcode_name(OP_RETURN),         "OP_RETURN");
    EXPECT_STR(cando_opcode_name(OP_CALL),           "OP_CALL");
    EXPECT_STR(cando_opcode_name(OP_DEF_GLOBAL),     "OP_DEF_GLOBAL");
    EXPECT_STR(cando_opcode_name(OP_RANGE_ASC),      "OP_RANGE_ASC");
    EXPECT_STR(cando_opcode_name(OP_PIPE_INIT),      "OP_PIPE_INIT");
}

/* -------------------------------------------------------------------------
 * Tests: compile success / failure
 * ------------------------------------------------------------------------ */
TEST(test_empty_source_compiles)
{
    CompileResult r = compile("");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.chunk->code_len, 1u); /* just OP_HALT */
    EXPECT_EQ(r.chunk->code[0], (u8)OP_HALT);
    cando_chunk_free(r.chunk);
}

TEST(test_parse_error_reported)
{
    CompileResult r = compile("VAR = ;"); /* missing name */
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(cando_parser_error(&r.parser) != NULL);
    cando_chunk_free(r.chunk);
}

/* -------------------------------------------------------------------------
 * Tests: literal expressions
 * ------------------------------------------------------------------------ */
TEST(test_number_literal)
{
    /* "42;" → OP_CONST(3 bytes) + OP_POP(1) + OP_HALT(1) = 5 bytes */
    CandoChunk *c = compile_ok("42;");
    EXPECT_EQ(c->code[0], (u8)OP_CONST);
    EXPECT_EQ(c->code[1], 0u);  /* constant index lo byte = 0 */
    EXPECT_TRUE(const_is_number(c, 0, 42.0));
    EXPECT_EQ(c->code[3], (u8)OP_POP);
    EXPECT_EQ(c->code[4], (u8)OP_HALT);
    cando_chunk_free(c);
}

TEST(test_float_literal)
{
    CandoChunk *c = compile_ok("3.14;");
    EXPECT_EQ(c->code[0], (u8)OP_CONST);
    EXPECT_TRUE(const_is_number(c, 0, 3.14));
    cando_chunk_free(c);
}

TEST(test_null_literal)
{
    CandoChunk *c = compile_ok("NULL;");
    EXPECT_EQ(c->code[0], (u8)OP_NULL);
    cando_chunk_free(c);
}

TEST(test_true_false_literals)
{
    CandoChunk *c = compile_ok("TRUE; FALSE;");
    EXPECT_EQ(c->code[0], (u8)OP_TRUE);
    EXPECT_EQ(c->code[1], (u8)OP_POP);
    EXPECT_EQ(c->code[2], (u8)OP_FALSE);
    cando_chunk_free(c);
}

TEST(test_string_literal)
{
    CandoChunk *c = compile_ok("\"hello\";");
    EXPECT_EQ(c->code[0], (u8)OP_CONST);
    EXPECT_TRUE(const_is_string(c, 0, "hello"));
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: arithmetic expressions
 * ------------------------------------------------------------------------ */
TEST(test_addition)
{
    /* "1 + 2;" → CONST(3) CONST(3) ADD(1) POP(1) HALT(1) = 9 bytes */
    CandoChunk *c = compile_ok("1 + 2;");
    EXPECT_EQ(c->code[0], (u8)OP_CONST);
    EXPECT_TRUE(const_is_number(c, 0, 1.0));
    EXPECT_EQ(c->code[3], (u8)OP_CONST);
    EXPECT_TRUE(const_is_number(c, 1, 2.0));
    EXPECT_EQ(c->code[6], (u8)OP_ADD);
    cando_chunk_free(c);
}

TEST(test_arithmetic_operators)
{
    struct { const char *src; CandoOpcode op; } cases[] = {
        { "5 - 3;", OP_SUB },
        { "4 * 2;", OP_MUL },
        { "8 / 2;", OP_DIV },
        { "7 % 3;", OP_MOD },
        { "2 ^ 8;", OP_POW },
    };
    for (usize i = 0; i < CANDO_ARRAY_LEN(cases); i++) {
        CandoChunk *c = compile_ok(cases[i].src);
        EXPECT_TRUE(find_op(c, cases[i].op) >= 0);
        cando_chunk_free(c);
    }
}

TEST(test_unary_negate)
{
    CandoChunk *c = compile_ok("-5;");
    EXPECT_TRUE(find_op(c, OP_NEG) >= 0);
    cando_chunk_free(c);
}

TEST(test_unary_not)
{
    CandoChunk *c = compile_ok("!TRUE;");
    EXPECT_TRUE(find_op(c, OP_NOT) >= 0);
    cando_chunk_free(c);
}

TEST(test_operator_precedence)
{
    /* "2 + 3 * 4;" — MUL before ADD */
    CandoChunk *c = compile_ok("2 + 3 * 4;");
    int mul_pos = find_op(c, OP_MUL);
    int add_pos = find_op(c, OP_ADD);
    EXPECT_TRUE(mul_pos >= 0 && add_pos >= 0);
    EXPECT_TRUE(mul_pos < add_pos);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: comparison and logical operators
 * ------------------------------------------------------------------------ */
TEST(test_comparison_operators)
{
    struct { const char *src; CandoOpcode op; } cases[] = {
        { "1 == 2;", OP_EQ  },
        { "1 != 2;", OP_NEQ },
        { "1 < 2;",  OP_LT  },
        { "1 > 2;",  OP_GT  },
        { "1 <= 2;", OP_LEQ },
        { "1 >= 2;", OP_GEQ },
    };
    for (usize i = 0; i < CANDO_ARRAY_LEN(cases); i++) {
        CandoChunk *c = compile_ok(cases[i].src);
        EXPECT_TRUE(find_op(c, cases[i].op) >= 0);
        cando_chunk_free(c);
    }
}

TEST(test_grouped_multi_eq)
{
    /* (x == 1, 3, 5) inside a function arg must emit OP_EQ_STACK, not error */
    CandoChunk *c = compile_ok("foo((x == 1, 3, 5));");
    EXPECT_TRUE(find_op(c, OP_EQ_STACK) >= 0);
    cando_chunk_free(c);

    /* Same pattern at top level should also work */
    c = compile_ok("(x == 2, 4, 6);");
    EXPECT_TRUE(find_op(c, OP_EQ_STACK) >= 0);
    cando_chunk_free(c);
}

TEST(test_logical_and)
{
    /* AND uses OP_AND_JUMP for short-circuit */
    CandoChunk *c = compile_ok("TRUE && FALSE;");
    EXPECT_TRUE(find_op(c, OP_AND_JUMP) >= 0);
    EXPECT_TRUE(find_op(c, OP_POP) >= 0);
    cando_chunk_free(c);
}

TEST(test_logical_or)
{
    /* OR uses OP_OR_JUMP for short-circuit */
    CandoChunk *c = compile_ok("TRUE || FALSE;");
    EXPECT_TRUE(find_op(c, OP_OR_JUMP) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: variable declarations
 * ------------------------------------------------------------------------ */
TEST(test_var_declaration)
{
    /* "VAR x = 10;" at global scope → CONST(10), DEF_GLOBAL(name_idx) */
    CandoChunk *c = compile_ok("VAR x = 10;");
    EXPECT_TRUE(find_op(c, OP_DEF_GLOBAL) >= 0);
    bool found = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (cando_is_number(c->constants[i]) && c->constants[i].as.number == 10.0)
            found = true;
    }
    EXPECT_TRUE(found);
    cando_chunk_free(c);
}

TEST(test_const_declaration)
{
    CandoChunk *c = compile_ok("CONST y = 42;");
    EXPECT_TRUE(find_op(c, OP_DEF_CONST_GLOBAL) >= 0);
    cando_chunk_free(c);
}

TEST(test_var_load)
{
    CandoChunk *c = compile_ok("VAR x = 1; x;");
    EXPECT_TRUE(find_op(c, OP_DEF_GLOBAL) >= 0);
    EXPECT_TRUE(find_op(c, OP_LOAD_GLOBAL) >= 0);
    cando_chunk_free(c);
}

TEST(test_var_store_assignment)
{
    CandoChunk *c = compile_ok("VAR x = 1; x = 2;");
    EXPECT_TRUE(find_op(c, OP_DEF_GLOBAL) >= 0);
    EXPECT_TRUE(find_op(c, OP_STORE_GLOBAL) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: local variable scope
 * ------------------------------------------------------------------------ */
TEST(test_local_var_in_block)
{
    /* VAR declared inside a block → OP_DEF_LOCAL (local slot) */
    CandoChunk *c = compile_ok("IF TRUE { VAR x = 1; }");
    EXPECT_TRUE(find_op(c, OP_DEF_LOCAL) >= 0);
    cando_chunk_free(c);
}

TEST(test_local_var_load_store)
{
    CandoChunk *c = compile_ok("IF TRUE { VAR x = 1; x = 2; }");
    EXPECT_TRUE(find_op(c, OP_DEF_LOCAL) >= 0);
    EXPECT_TRUE(find_op(c, OP_STORE_LOCAL) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: control flow
 * ------------------------------------------------------------------------ */
TEST(test_if_statement)
{
    CandoChunk *c = compile_ok("IF TRUE { NULL; }");
    EXPECT_TRUE(find_op(c, OP_JUMP_IF_FALSE) >= 0);
    EXPECT_TRUE(find_op(c, OP_JUMP) >= 0);
    cando_chunk_free(c);
}

TEST(test_if_else)
{
    CandoChunk *c = compile_ok("IF TRUE { 1; } ELSE { 2; }");
    int jif = count_op(c, OP_JUMP_IF_FALSE);
    int j   = count_op(c, OP_JUMP);
    EXPECT_EQ(jif, 1);
    EXPECT_EQ(j,   1);
    cando_chunk_free(c);
}

TEST(test_while_loop)
{
    CandoChunk *c = compile_ok("WHILE FALSE { 1; }");
    EXPECT_TRUE(find_op(c, OP_JUMP_IF_FALSE) >= 0);
    EXPECT_TRUE(find_op(c, OP_LOOP) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: function and return
 * ------------------------------------------------------------------------ */
TEST(test_function_declaration)
{
    CandoChunk *c = compile_ok("FUNCTION add(a, b) { RETURN a; }");
    EXPECT_TRUE(find_op(c, OP_JUMP) >= 0);
    EXPECT_TRUE(find_op(c, OP_RETURN) >= 0);
    EXPECT_TRUE(find_op(c, OP_DEF_GLOBAL) >= 0);
    /* Function name 'add' in constants pool */
    bool found = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (const_is_string(c, i, "add")) { found = true; break; }
    }
    EXPECT_TRUE(found);
    cando_chunk_free(c);
}

TEST(test_function_call)
{
    CandoChunk *c = compile_ok("foo(1, 2);");
    EXPECT_TRUE(find_op(c, OP_CALL) >= 0);
    /* argc = 2; OP_CALL is OPFMT_A (u16 little-endian), lo byte = 2 */
    int call_pos = find_op(c, OP_CALL);
    EXPECT_TRUE(call_pos >= 0 && (u32)(call_pos + 1) < c->code_len);
    EXPECT_EQ(c->code[call_pos + 1], 2u);
    cando_chunk_free(c);
}

TEST(test_return_value)
{
    CandoChunk *c = compile_ok("FUNCTION f() { RETURN 99; }");
    EXPECT_TRUE(find_op(c, OP_RETURN) >= 0);
    bool found = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (const_is_number(c, i, 99.0)) { found = true; break; }
    }
    EXPECT_TRUE(found);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: array and object literals
 * ------------------------------------------------------------------------ */
TEST(test_array_literal)
{
    CandoChunk *c = compile_ok("[1, 2, 3];");
    int pos = find_op(c, OP_NEW_ARRAY);
    EXPECT_TRUE(pos >= 0);
    /* OP_NEW_ARRAY is OPFMT_A; lo byte of u16(3) = 3 */
    EXPECT_EQ(c->code[pos + 1], 3u);
    cando_chunk_free(c);
}

TEST(test_empty_array)
{
    CandoChunk *c = compile_ok("[];");
    int pos = find_op(c, OP_NEW_ARRAY);
    EXPECT_TRUE(pos >= 0);
    EXPECT_EQ(c->code[pos + 1], 0u);
    cando_chunk_free(c);
}

TEST(test_object_literal)
{
    /* OP_NEW_OBJECT (no args) + OP_SET_FIELD for each key-value pair */
    CandoChunk *c = compile_ok("VAR obj = {x: 1, y: 2};");
    EXPECT_TRUE(find_op(c, OP_NEW_OBJECT) >= 0);
    /* Two SET_FIELD instructions for x and y */
    EXPECT_EQ(count_op(c, OP_SET_FIELD), 2);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: property access and method call
 * ------------------------------------------------------------------------ */
TEST(test_property_get)
{
    CandoChunk *c = compile_ok("obj.name;");
    int pos = find_op(c, OP_GET_FIELD);
    EXPECT_TRUE(pos >= 0);
    bool found = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (const_is_string(c, i, "name")) { found = true; break; }
    }
    EXPECT_TRUE(found);
    cando_chunk_free(c);
}

TEST(test_fluent_call)
{
    CandoChunk *c = compile_ok("obj::push(1);");
    EXPECT_TRUE(find_op(c, OP_FLUENT_CALL) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: throw and try/catch
 * ------------------------------------------------------------------------ */
TEST(test_throw_statement)
{
    CandoChunk *c = compile_ok("THROW \"error\";");
    EXPECT_TRUE(find_op(c, OP_THROW) >= 0);
    cando_chunk_free(c);
}

TEST(test_try_catch)
{
    CandoChunk *c = compile_ok("TRY { 1; } CATCH (e) { 2; }");
    EXPECT_TRUE(find_op(c, OP_TRY_BEGIN) >= 0);
    EXPECT_TRUE(find_op(c, OP_TRY_END) >= 0);
    /* Catch variable is bound as a local slot; verify CATCH_BEGIN emitted */
    EXPECT_TRUE(find_op(c, OP_CATCH_BEGIN) >= 0);
    cando_chunk_free(c);
}

TEST(test_try_catch_finaly)
{
    CandoChunk *c = compile_ok("TRY { 1; } CATCH (err) { 2; } FINALY { 3; }");
    EXPECT_TRUE(find_op(c, OP_TRY_BEGIN) >= 0);
    EXPECT_TRUE(find_op(c, OP_TRY_END) >= 0);
    bool has1 = false, has2 = false, has3 = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (!cando_is_number(c->constants[i])) continue;
        f64 n = c->constants[i].as.number;
        if (n == 1.0) has1 = true;
        if (n == 2.0) has2 = true;
        if (n == 3.0) has3 = true;
    }
    EXPECT_TRUE(has1 && has2 && has3);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: pipe and filter operators
 * ------------------------------------------------------------------------ */
TEST(test_pipe_operator)
{
    CandoChunk *c = compile_ok("data ~> pipe * 2;");
    EXPECT_TRUE(find_op(c, OP_PIPE_INIT) >= 0);
    EXPECT_TRUE(find_op(c, OP_PIPE_NEXT) >= 0);
    EXPECT_TRUE(find_op(c, OP_PIPE_END) >= 0);
    cando_chunk_free(c);
}

TEST(test_filter_operator)
{
    CandoChunk *c = compile_ok("items ~!> pipe > 0;");
    EXPECT_TRUE(find_op(c, OP_PIPE_INIT) >= 0);
    EXPECT_TRUE(find_op(c, OP_FILTER_NEXT) >= 0);
    EXPECT_TRUE(find_op(c, OP_PIPE_END) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: range operators
 * ------------------------------------------------------------------------ */
TEST(test_range_asc)
{
    CandoChunk *c = compile_ok("1 -> 10;");
    EXPECT_TRUE(find_op(c, OP_RANGE_ASC) >= 0);
    cando_chunk_free(c);
}

TEST(test_range_desc)
{
    CandoChunk *c = compile_ok("10 <- 1;");
    EXPECT_TRUE(find_op(c, OP_RANGE_DESC) >= 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: class declaration
 * ------------------------------------------------------------------------ */
TEST(test_class_declaration)
{
    CandoChunk *c = compile_ok("CLASS Dog { FUNCTION bark() { RETURN NULL; } }");
    EXPECT_TRUE(find_op(c, OP_NEW_CLASS) >= 0);
    EXPECT_TRUE(find_op(c, OP_BIND_METHOD) >= 0);
    bool found = false;
    for (u32 i = 0; i < c->const_count; i++) {
        if (const_is_string(c, i, "Dog")) { found = true; break; }
    }
    EXPECT_TRUE(found);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: compound statements
 * ------------------------------------------------------------------------ */
TEST(test_nested_if_while)
{
    const char *src =
        "VAR i = 0;\n"
        "WHILE i < 10 {\n"
        "    IF i == 5 { BREAK; }\n"
        "    i = i + 1;\n"
        "}";
    CompileResult r = compile(src);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(find_op(r.chunk, OP_LOOP) >= 0);
    cando_chunk_free(r.chunk);
}

TEST(test_multiple_statements)
{
    const char *src =
        "VAR a = 1;\n"
        "VAR b = 2;\n"
        "VAR c = a + b;\n"
        "c;";
    CompileResult r = compile(src);
    EXPECT_TRUE(r.ok);
    /* Constants pool must contain "a", "b", "c" and numbers 1, 2 */
    bool has_a = false, has_b = false, has_c = false;
    bool has_1 = false, has_2 = false;
    for (u32 i = 0; i < r.chunk->const_count; i++) {
        if (const_is_string(r.chunk, i, "a")) has_a = true;
        if (const_is_string(r.chunk, i, "b")) has_b = true;
        if (const_is_string(r.chunk, i, "c")) has_c = true;
        if (const_is_number(r.chunk, i, 1.0)) has_1 = true;
        if (const_is_number(r.chunk, i, 2.0)) has_2 = true;
    }
    EXPECT_TRUE(has_a && has_b && has_c && has_1 && has_2);
    EXPECT_TRUE(find_op(r.chunk, OP_DEF_GLOBAL) >= 0);
    EXPECT_TRUE(find_op(r.chunk, OP_ADD) >= 0);
    cando_chunk_free(r.chunk);
}

/* -------------------------------------------------------------------------
 * Tests: multi-return syntax
 * ------------------------------------------------------------------------ */
TEST(test_multi_return)
{
    /* RETURN with multiple values should emit OP_RETURN with count>1 */
    CandoChunk *c = compile_ok("FUNCTION f() { RETURN 1, 2; }");
    EXPECT_TRUE(find_op(c, OP_RETURN) >= 0);
    /* Find OP_RETURN and check its operand = 2 */
    int ret_pos = find_op(c, OP_RETURN);
    EXPECT_TRUE(ret_pos >= 0);
    if (ret_pos >= 0 && (u32)(ret_pos + 1) < c->code_len)
        EXPECT_EQ(c->code[ret_pos + 1], 2u);  /* count=2, low byte */
    cando_chunk_free(c);
}

TEST(test_multi_return_three)
{
    CandoChunk *c = compile_ok("FUNCTION f() { RETURN 1, 2, 3; }");
    int ret_pos = find_op(c, OP_RETURN);
    EXPECT_TRUE(ret_pos >= 0);
    if (ret_pos >= 0 && (u32)(ret_pos + 1) < c->code_len)
        EXPECT_EQ(c->code[ret_pos + 1], 3u);  /* count=3 */
    cando_chunk_free(c);
}

TEST(test_single_return_unchanged)
{
    /* Existing single RETURN should still work (count=1) */
    CandoChunk *c = compile_ok("FUNCTION f() { RETURN 42; }");
    int ret_pos = find_op(c, OP_RETURN);
    EXPECT_TRUE(ret_pos >= 0);
    if (ret_pos >= 0 && (u32)(ret_pos + 1) < c->code_len)
        EXPECT_EQ(c->code[ret_pos + 1], 1u);
    cando_chunk_free(c);
}

TEST(test_call_with_spread)
{
    /* f(g()) where g is a call — should emit OP_SPREAD_RET after inner call */
    CandoChunk *c = compile_ok("f(g());");
    EXPECT_TRUE(find_op(c, OP_CALL) >= 0);
    EXPECT_TRUE(find_op(c, OP_SPREAD_RET) >= 0);
    cando_chunk_free(c);
}

TEST(test_call_no_spread_literal)
{
    /* f(1, 2) — no calls as args, no SPREAD_RET */
    CandoChunk *c = compile_ok("f(1, 2);");
    EXPECT_EQ(count_op(c, OP_SPREAD_RET), 0);
    cando_chunk_free(c);
}

/* Regression: Issue #17 -- an anonymous function expression used as an inline
 * argument must not be treated as a call.  Previously, if the body's last
 * emitted expression happened to be a call (e.g. `function() { return o.m(); }`)
 * the parser would leave last_expr_was_call=true after OP_CLOSURE and the
 * surrounding parse_call would spuriously emit OP_SPREAD_RET, corrupting
 * argc at runtime (often crashing).  The fix clears that flag after
 * OP_CLOSURE.                                                              */
TEST(test_inline_function_no_spread_from_body)
{
    /* Body contains a dot-form call -- stresses the original crash case.
     * Neither the outer call nor the inner return should spread: the
     * closure is a plain value, and `return <call>` emits OP_RETURN
     * directly without a preceding OP_SPREAD_RET.                         */
    CandoChunk *c = compile_ok("f(function() { return o.m(); });");
    EXPECT_EQ(count_op(c, OP_SPREAD_RET), 0);
    /* Find the *outer* OP_CALL (the last one); its argc must be 1. */
    int call_pos = -1;
    u32 i = 0;
    while (i < c->code_len) {
        CandoOpcode cur = (CandoOpcode)c->code[i];
        if (cur == OP_CALL) call_pos = (int)i;
        u32 sz = cando_opcode_size(cur);
        i += (sz > 0) ? sz : 1;
    }
    EXPECT_TRUE(call_pos >= 0);
    EXPECT_EQ(c->code[call_pos + 1], 1u);
    cando_chunk_free(c);
}

TEST(test_inline_function_body_with_method_call)
{
    /* Body contains a colon-form method call. */
    CandoChunk *c = compile_ok("f(function() { return arr:length(); });");
    /* Outer f(...) has exactly 1 argument slot (the closure). */
    int call_pos = -1;
    u32 i = 0;
    while (i < c->code_len) {
        CandoOpcode cur = (CandoOpcode)c->code[i];
        if (cur == OP_CALL) call_pos = (int)i;
        u32 sz = cando_opcode_size(cur);
        i += (sz > 0) ? sz : 1;
    }
    EXPECT_TRUE(call_pos >= 0);
    EXPECT_EQ(c->code[call_pos + 1], 1u);
    cando_chunk_free(c);
}

TEST(test_inline_function_no_trailing_spread_empty_body)
{
    /* function(){} as an inline argument must never emit OP_SPREAD_RET
     * around the outer call (the closure is a plain value).               */
    CandoChunk *c = compile_ok("f(function() {});");
    EXPECT_EQ(count_op(c, OP_SPREAD_RET), 0);
    cando_chunk_free(c);
}

TEST(test_inline_function_variable_form_equivalent)
{
    /* Variable-bound form -- used to be the only working form -- must also
     * produce no spurious OP_SPREAD_RET around the outer call.             */
    CandoChunk *c = compile_ok(
        "VAR fn = function() { return o.m(); }; f(fn);");
    EXPECT_EQ(count_op(c, OP_SPREAD_RET), 0);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * Tests: multi-variable declaration
 * ------------------------------------------------------------------------ */
TEST(test_multi_var_decl)
{
    /* VAR a, b = 1, 2 should compile successfully */
    CandoChunk *c = compile_ok("VAR a, b = 1, 2;");
    /* Should emit 2 DEF_GLOBAL opcodes */
    EXPECT_EQ(count_op(c, OP_DEF_GLOBAL), 2);
    cando_chunk_free(c);
}

TEST(test_multi_var_decl_single_expr)
{
    /* VAR x, y = getPos(); — single call expr for 2 vars */
    CandoChunk *c = compile_ok("VAR x, y = getPos();");
    /* Should emit SPREAD_RET (since getPos() is a call) */
    EXPECT_TRUE(find_op(c, OP_SPREAD_RET) >= 0);
    EXPECT_EQ(count_op(c, OP_DEF_GLOBAL), 2);
    cando_chunk_free(c);
}

TEST(test_multi_var_decl_local)
{
    /* Local multi-var inside a block */
    CandoChunk *c = compile_ok("{ VAR a, b = 10, 20; }");
    EXPECT_EQ(count_op(c, OP_DEF_LOCAL), 2);
    cando_chunk_free(c);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */
int main(void)
{
    printf("\n=== Cando parser unit tests ===\n\n");

    printf("-- chunk management --\n");
    run_test("chunk init/free",                test_chunk_init_free);
    run_test("chunk write bytes",              test_chunk_write_bytes);
    run_test("chunk add constant",             test_chunk_add_constant);
    run_test("chunk patch jump (little-endian)", test_chunk_patch_jump);

    printf("\n-- opcode names --\n");
    run_test("opcode names",                   test_opcode_names);

    printf("\n-- compile success/failure --\n");
    run_test("empty source compiles",          test_empty_source_compiles);
    run_test("parse error reported",           test_parse_error_reported);

    printf("\n-- literal expressions --\n");
    run_test("number literal",                 test_number_literal);
    run_test("float literal",                  test_float_literal);
    run_test("null literal",                   test_null_literal);
    run_test("true/false literals",            test_true_false_literals);
    run_test("string literal",                 test_string_literal);

    printf("\n-- arithmetic --\n");
    run_test("addition",                       test_addition);
    run_test("arithmetic operators",           test_arithmetic_operators);
    run_test("unary negate",                   test_unary_negate);
    run_test("unary not",                      test_unary_not);
    run_test("operator precedence",            test_operator_precedence);

    printf("\n-- comparison / logical --\n");
    run_test("comparison operators",           test_comparison_operators);
    run_test("grouped multi-equality (==)",    test_grouped_multi_eq);
    run_test("logical AND short-circuit",      test_logical_and);
    run_test("logical OR short-circuit",       test_logical_or);

    printf("\n-- variables --\n");
    run_test("var declaration",                test_var_declaration);
    run_test("const declaration",              test_const_declaration);
    run_test("var load",                       test_var_load);
    run_test("var store assignment",           test_var_store_assignment);
    run_test("local var in block",             test_local_var_in_block);
    run_test("local var load/store",           test_local_var_load_store);

    printf("\n-- control flow --\n");
    run_test("if statement",                   test_if_statement);
    run_test("if/else statement",              test_if_else);
    run_test("while loop",                     test_while_loop);

    printf("\n-- functions --\n");
    run_test("function declaration",           test_function_declaration);
    run_test("function call",                  test_function_call);
    run_test("return value",                   test_return_value);

    printf("\n-- arrays / objects --\n");
    run_test("array literal",                  test_array_literal);
    run_test("empty array",                    test_empty_array);
    run_test("object literal",                 test_object_literal);

    printf("\n-- property access --\n");
    run_test("property get",                   test_property_get);
    run_test("fluent call",                    test_fluent_call);

    printf("\n-- error handling --\n");
    run_test("throw statement",                test_throw_statement);
    run_test("try/catch",                      test_try_catch);
    run_test("try/catch/finaly",               test_try_catch_finaly);

    printf("\n-- pipe / filter --\n");
    run_test("pipe operator (~>)",             test_pipe_operator);
    run_test("filter operator (~!>)",          test_filter_operator);

    printf("\n-- ranges --\n");
    run_test("ascending range (->)",           test_range_asc);
    run_test("descending range (<-)",          test_range_desc);

    printf("\n-- classes --\n");
    run_test("class declaration",              test_class_declaration);

    printf("\n-- compound --\n");
    run_test("nested if/while",                test_nested_if_while);
    run_test("multiple statements",            test_multiple_statements);

    printf("\n-- multi-return --\n");
    run_test("multi return (2 values)",        test_multi_return);
    run_test("multi return (3 values)",        test_multi_return_three);
    run_test("single return unchanged",        test_single_return_unchanged);
    run_test("call with spread (SPREAD_RET)",  test_call_with_spread);
    run_test("call no spread (literal args)",  test_call_no_spread_literal);
    run_test("inline fn arg: body call no spread (#17)",
                                               test_inline_function_no_spread_from_body);
    run_test("inline fn arg: method call body (#17)",
                                               test_inline_function_body_with_method_call);
    run_test("inline fn arg: empty body no spread (#17)",
                                               test_inline_function_no_trailing_spread_empty_body);
    run_test("inline fn arg: variable form equiv (#17)",
                                               test_inline_function_variable_form_equivalent);

    printf("\n-- multi-variable declaration --\n");
    run_test("multi-var global decl",          test_multi_var_decl);
    run_test("multi-var single expr (spread)", test_multi_var_decl_single_expr);
    run_test("multi-var local decl",           test_multi_var_decl_local);

    printf("\n=== Results: %d/%d passed ===\n",
           g_tests_passed, g_tests_run);

    return g_tests_failed > 0 ? 1 : 0;
}
