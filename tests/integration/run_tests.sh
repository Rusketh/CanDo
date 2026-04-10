#!/usr/bin/env bash
# run_tests.sh -- Integration test runner for the cando executable.
#
# Runs each .cdo script through ./cando and checks output against expected
# results.  Must be invoked from the repository root.
#
# Exit status: 0 if all tests pass, 1 if any fail.

set -euo pipefail

CANDO="${1:-./cando}"
SCRIPTS="tests/scripts"
PASS=0
FAIL=0

# Detect Windows (MSYS2/MinGW) vs Unix for OS-sensitive expected values.
if [[ "${MSYSTEM:-}" == MINGW* ]] || [[ "${OS:-}" == "Windows_NT" ]]; then
    PLATFORM_OS_NAME="windows"
else
    PLATFORM_OS_NAME="unix"
fi

run_test() {
    local name="$1"
    local script="$2"
    local expected="$3"
    local actual
    actual=$("$CANDO" "$script" 2>&1)
    if [ "$actual" = "$expected" ]; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        echo "        expected: $(printf '%s' "$expected" | head -5)"
        echo "        actual:   $(printf '%s' "$actual"   | head -5)"
        FAIL=$((FAIL + 1))
    fi
}

# run_smoke: only verifies exit code 0 (output is dynamic or informational).
run_smoke() {
    local name="$1"
    local script="$2"
    if "$CANDO" "$script" > /dev/null 2>&1; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "cando integration tests"
echo "-----------------------"

run_test "hello" "$SCRIPTS/hello.cdo" "hello world"

run_test "arithmetic" "$SCRIPTS/arithmetic.cdo" \
    "$(printf '3\n7\n20\n5\n1\n256\n-5\n7\n14\n20\n11\n10\n15\n12\n24\n6\n2\n2\n7\n16\n8\n27')"

run_test "variables" "$SCRIPTS/variables.cdo" \
    "$(printf '42\n3\n99\n10\n20\n77\n11')"

run_test "comparison" "$SCRIPTS/comparison.cdo" \
    "$(printf 'true\ntrue\ntrue\ntrue\ntrue\ntrue\nfalse\ntrue\nfalse\ntrue\nfalse\ntrue\nfalse\ntrue\nfalse\ntrue\ngt_multi_pass\ngt_multi_fail\nlt_multi_pass\neq_multi_pass\neq_multi_fail\nfirst_only_pass\nspread_all_fail\nspread_gt_pass\nspread_lt_fail\nspread_eq_pass\nspread_neq_fail\nspread_geq_pass\nspread_leq_fail\nmask_gt_pass\nmask_lt_fail\nmask_all_fail\nlist_call_pass\nlist_call_fail')"

run_test "if_else" "$SCRIPTS/if_else.cdo" \
    "$(printf 'if_true\nelse_branch\nA\nB\nC\nF\nfirst\nsecond\nbig_positive\nscoped')"

run_test "while" "$SCRIPTS/while.cdo" \
    "$(printf '0\n1\n2\n3\n4\n3\n2\n1\n15\n11\n12\n21\n22')"

run_test "for_loops" "$SCRIPTS/for_loops.cdo" \
    "$(printf '1\n2\n3\n4\n3\n2\n1\n100\n200\n300\n15\n11\n12\n21\n22\n1\n4\n9')"

run_test "for_over" "$SCRIPTS/for_over.cdo" \
    "$(printf -- '--- Array iteration ---\n0 10\n1 20\n2 30\n--- Triple variable iteration ---\n0 10 20\n1 20 40\n2 30 60\n--- Short variables (padding with null) ---\n0\n1\n2\n--- Extra variables (filling with null) ---\n0 10 null\n1 20 null\n2 30 null\n--- Single return iterator ---\n10\n20\n30\n--- Many return values (16 limit) ---\n1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n--- Multi-return padding ---\nonly_one null null\nDONE')"

run_test "functions" "$SCRIPTS/functions.cdo" \
    "$(printf 'hello from function\n7\n30\n0\n1\n8\n3\n7\n12\n5\n8')"

run_test "strings" "$SCRIPTS/strings.cdo" \
    "$(printf 'hello world\n5\nstring\nnumber\nbool\nnull\n42\n3.1400000000000001\ntrue\ntrue\nfalse\ntrue\n5\nworld\nhello world\nHELLO WORLD\nhi\nabc\ndef\nababab\n6\na\nb\nc\ncount: 99')"

run_test "arrays" "$SCRIPTS/arrays.cdo" \
    "$(printf '3\n10\n20\n30\n99\n15\n1\n2\n3\n4\n0\n1\n4\n9\n15')"

run_test "objects" "$SCRIPTS/objects.cdo" \
    "$(printf 'Alice\n30\nNYC\n31\nBob\nMain St\n12345\nEve age 25\n3\n4')"

run_test "method_call" "$SCRIPTS/method_call.cdo" \
    "$(printf 't:meth():\n100\nt::meth():\n100\nChained fluent:\n300\ns:toUpper():\nHELLO\nFluent string equal:\ntrue')"

run_test "try_catch" "$SCRIPTS/try_catch.cdo" \
    "$(printf 'caught: oops\n404\nnot found\nonly one\ntrue\n1\ncatch: boom\nfinally\ntry_body\nfinaly_no_throw\ndiv_by_zero_caught\nrethrown: inner\n30')"

run_test "math_lib" "$SCRIPTS/math_lib.cdo" \
    "$(printf '7\n3\n3\n4\n3\n2\n5\n2\n1\n8\n10\n0\n5\n1024\n0\npi_ok\ne_ok')"

run_test "pipe" "$SCRIPTS/pipe.cdo" \
    "$(printf '10\n20\n50\n2\n6\n2\n4\n4\n5\n1\n5\n1\n1\n2\n2\n3\n1\n3')"

run_test "eval" "$SCRIPTS/eval.cdo" \
    "$(printf '6\n50\nfrom_sandbox\n10\ncontained\n42')"

run_test "include" "$SCRIPTS/include.cdo" \
    "$(printf 'Hello, World!\nHello, Cando!\ntrue\n1\n3\n4\nHello, Cache!')"

run_test "include_16" "$SCRIPTS/include_16.cdo" \
    "$(printf '1\n8\n16\ntrue\ntrue\ntrue')"

run_test "threads" "$SCRIPTS/threads.cdo" \
    "$(printf '42\n10\n20\ntrue\nsleep_ok\nid_ok\n99\ntrue\nnull\n7\n1\n2\n3\ndone\nerror\nbad\ntrue\ntrue\n77\n88\ncaught_err\nalready\nfalse')"

run_test "lib_os" "$SCRIPTS/lib_os.cdo" \
    "$(printf 'os.name: %s\nos_time_ok\nos_clock_ok\nPATH_ok\nCANDO_TEST: Hello' "$PLATFORM_OS_NAME")"

run_test "lib_datetime" "$SCRIPTS/lib_datetime.cdo" \
    "$(printf 'now_ok\nFormatted: 2023-10-27\nparse_ok')"

run_test "lib_array" "$SCRIPTS/lib_array.cdo" \
    "$(printf '3\n4\n4\n3\n2\n4\n6\n4\n6\n6')"

run_test "lib_object" "$SCRIPTS/lib_object.cdo" \
    "$(printf '1\n99\n2\n1\n2\n3\n10\n99\n30\n20\nalice\nbob\nhello\nc\na\nb\n3\n1\n2\nfalse\ntrue\nfalse')"

run_test "lib_crypto" "$SCRIPTS/lib_crypto.cdo" \
    "$(printf 'md5_ok\nsha256_ok\naGVsbG8gd29ybGQA\nhello world')"

run_test "lib_sys" "$SCRIPTS/lib_sys.cdo" \
    "$(printf 'pid_ok\nppid_ok')"

run_test "lib_enhance" "$SCRIPTS/lib_enhance.cdo" \
    "$(printf "math.log10(100):  2\nmath.exp(0):  1\nstartsWith('hello'):  true\nendsWith('world'):  true\nreplace('world', 'cando'): hello cando\nformat: Hello Alice, age 30")"

run_test "break_continue" "$SCRIPTS/break_continue.cdo" \
    "$(printf '0\n1\n2\n1\n2\n4\n5\n10\n20\n10\n20\n40\n50\n0\n1\n0\n1\n2\n0\n1\n3\n4\n0\n10\n20\ndone')"

run_smoke "test_array"          "$SCRIPTS/test_array.cdo"
run_smoke "test_array_ext"      "$SCRIPTS/test_array_ext.cdo"
run_smoke "test_race_conditions" "$SCRIPTS/test_race_conditions.cdo"
run_smoke "test_crypto"   "$SCRIPTS/test_crypto.cdo"
run_smoke "test_datetime" "$SCRIPTS/test_datetime.cdo"
run_smoke "test_enhance"  "$SCRIPTS/test_enhance.cdo"
run_smoke "test_os"       "$SCRIPTS/test_os.cdo"
run_smoke "test_sys"      "$SCRIPTS/test_sys.cdo"

echo "-----------------------"
echo "Results: $PASS passed, $FAIL failed"

[ "$FAIL" -eq 0 ]
