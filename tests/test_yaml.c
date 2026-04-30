/*
 * tests/test_yaml.c -- Unit tests for the yaml standard library.
 *
 * Exercises both the C-level entry point (cando_lib_yaml_parse_buffer) and
 * the script-visible interface (yaml.parse / yaml.stringify) by spinning up
 * a real CandoVM, opening libraries, and running short scripts.
 *
 * Compile via Makefile:
 *   make test_yaml
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "cando.h"

#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Minimal harness mirroring test_sockutil / test_vm
 * --------------------------------------------------------------------- */
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond) \
    do { \
        g_run++; \
        if (cond) { g_passed++; } \
        else { g_failed++; \
               fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_EQ(a, b)  EXPECT((a) == (b))

#define TEST(name) static void name(CandoVM *vm)

static void run_test(const char *name, void (*fn)(CandoVM *), CandoVM *vm)
{
    printf("  %-52s ", name);
    fflush(stdout);
    int before = g_failed;
    /* Reset error state between tests by recreating the VM if needed. */
    fn(vm);
    printf("%s\n", g_failed == before ? "OK" : "FAIL");
}

/* Helper: run a short script that *should* succeed.  Asserts return code
 * and clears any leftover error. */
static bool run_ok(CandoVM *vm, const char *src)
{
    int rc = cando_dostring(vm, src, "test");
    if (rc != CANDO_OK) {
        fprintf(stderr, "  [unexpected error] %s\n", cando_errmsg(vm));
        return false;
    }
    return true;
}

/* =========================================================================
 * Tests: parse_buffer C API
 * ===================================================================== */

TEST(test_parse_null_via_buffer)
{
    /* Empty document parses to null. */
    CandoValue out = cando_null();
    bool ok = cando_lib_yaml_parse_buffer(vm, "", 0, "test", &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ((int)out.tag, (int)TYPE_NULL);
    cando_value_release(out);
}

TEST(test_parse_scalar_int)
{
    CandoValue out = cando_null();
    bool ok = cando_lib_yaml_parse_buffer(vm, "42", 2, "test", &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ((int)out.tag, (int)TYPE_NUMBER);
    EXPECT_EQ((int)out.as.number, 42);
    cando_value_release(out);
}

TEST(test_parse_scalar_bool)
{
    CandoValue out = cando_null();
    bool ok = cando_lib_yaml_parse_buffer(vm, "yes", 3, "test", &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ((int)out.tag, (int)TYPE_BOOL);
    EXPECT_TRUE(out.as.boolean);
    cando_value_release(out);
}

TEST(test_parse_block_map)
{
    const char *src = "name: alice\nage: 30\n";
    CandoValue out = cando_null();
    bool ok = cando_lib_yaml_parse_buffer(vm, src, strlen(src), "test", &out);
    EXPECT_TRUE(ok);
    EXPECT_EQ((int)out.tag, (int)TYPE_OBJECT);
    cando_value_release(out);
}

TEST(test_parse_error_message)
{
    /* Unterminated flow sequence; parse_buffer should set a VM error. */
    CandoValue out;
    bool ok = cando_lib_yaml_parse_buffer(vm, "[1, 2", 5, "test", &out);
    EXPECT_FALSE(ok);
    EXPECT_EQ((int)out.tag, (int)TYPE_NULL);
    EXPECT_TRUE(strstr(cando_errmsg(vm), "test") != NULL);
    /* Clear VM error so subsequent tests start clean. */
    vm->has_error = false;
    vm->error_msg[0] = '\0';
}

TEST(test_parse_tab_indent_rejected)
{
    /* Tab-indented block content is an error. */
    const char *src = "a:\n\tb: 1\n";
    CandoValue out;
    bool ok = cando_lib_yaml_parse_buffer(vm, src, strlen(src), "test", &out);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(strstr(cando_errmsg(vm), "tab") != NULL);
    vm->has_error = false;
    vm->error_msg[0] = '\0';
}

/* =========================================================================
 * Tests: yaml.parse / yaml.stringify via scripts
 * ===================================================================== */

TEST(test_script_parse_basic)
{
    EXPECT_TRUE(run_ok(vm,
        "VAR v = yaml.parse(\"a: 1\nb: hello\n\");\n"
        "IF v.a != 1         { THROW \"a wrong\"; }\n"
        "IF v.b != \"hello\"   { THROW \"b wrong\"; }\n"
    ));
}

TEST(test_script_parse_sequence)
{
    EXPECT_TRUE(run_ok(vm,
        "VAR v = yaml.parse(\"- one\n- 2\n- true\n\");\n"
        "IF v[0] != \"one\"  { THROW \"v[0]\"; }\n"
        "IF v[1] != 2       { THROW \"v[1]\"; }\n"
        "IF v[2] != true    { THROW \"v[2]\"; }\n"
    ));
}

TEST(test_script_round_trip)
{
    EXPECT_TRUE(run_ok(vm,
        "VAR orig = { name: \"Cando\", n: 3, ok: true, items: [1, 2] };\n"
        "VAR s    = yaml.stringify(orig);\n"
        "VAR back = yaml.parse(s);\n"
        "IF back.name != \"Cando\" { THROW \"name\"; }\n"
        "IF back.n    != 3         { THROW \"n\"; }\n"
        "IF back.ok   != true      { THROW \"ok\"; }\n"
        "IF back.items[0] != 1     { THROW \"items[0]\"; }\n"
        "IF back.items[1] != 2     { THROW \"items[1]\"; }\n"
    ));
}

TEST(test_script_quoted_scalar)
{
    /* Quoted scalar should remain a string even if it looks like a number. */
    EXPECT_TRUE(run_ok(vm,
        "VAR v = yaml.parse(\"'42'\");\n"
        "IF type(v) != \"string\" { THROW type(v); }\n"
        "IF v != \"42\"            { THROW v; }\n"
    ));
}

TEST(test_script_stringify_quotes_ambiguous)
{
    /* Strings that would round-trip to non-string types must be quoted. */
    EXPECT_TRUE(run_ok(vm,
        "VAR s = yaml.stringify(\"true\");\n"
        "IF s:find(`\"`) == null { THROW \"missing quotes: \" + s; }\n"
    ));
}

/* =========================================================================
 * Main
 * ===================================================================== */

int main(void)
{
    printf("Running yaml unit tests...\n");

    CandoVM *vm = cando_open();
    cando_openlibs(vm);

    run_test("test_parse_null_via_buffer",       test_parse_null_via_buffer,       vm);
    run_test("test_parse_scalar_int",            test_parse_scalar_int,            vm);
    run_test("test_parse_scalar_bool",           test_parse_scalar_bool,           vm);
    run_test("test_parse_block_map",             test_parse_block_map,             vm);
    run_test("test_parse_error_message",         test_parse_error_message,         vm);
    run_test("test_parse_tab_indent_rejected",   test_parse_tab_indent_rejected,   vm);
    run_test("test_script_parse_basic",          test_script_parse_basic,          vm);
    run_test("test_script_parse_sequence",       test_script_parse_sequence,       vm);
    run_test("test_script_round_trip",           test_script_round_trip,           vm);
    run_test("test_script_quoted_scalar",        test_script_quoted_scalar,        vm);
    run_test("test_script_stringify_quotes_ambiguous",
             test_script_stringify_quotes_ambiguous, vm);

    cando_close(vm);

    printf("\n  %d run, %d passed, %d failed\n", g_run, g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
