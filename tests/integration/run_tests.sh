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

run_test "for_loops_extra" "$SCRIPTS/for_loops_extra.cdo" \
    "$(printf 'x\ny\nz\n10\n20\n30\nok\n3\n0\n0\n1:1\n2:1\n3:1')"

run_test "jit_recorder" "$SCRIPTS/jit_recorder.cdo" \
    "$(printf '19900\n1\n0\n1\n1\n1\n1\ntrue\ntrue\n200\n1\ntrue\nfalse\ncaught\n20100\n19900\ntrue\n100\n0\ntrue\ntrue\n20500\ntrue\n40200\n1275\ntrue\n1000\nOP_CALL: recursion detected\n600\nloop inside inlined call (v1 limitation)\n100500\n20100\n40200\n20100\n10000\n200\n200\n20100\n338350\n20100\n60300\n200\nDONE')"

run_test "for_over" "$SCRIPTS/for_over.cdo" \
    "$(printf -- '--- Array iteration ---\n0 10\n1 20\n2 30\n--- Triple variable iteration ---\n0 10 20\n1 20 40\n2 30 60\n--- Short variables (padding with null) ---\n0\n1\n2\n--- Extra variables (filling with null) ---\n0 10 null\n1 20 null\n2 30 null\n--- Single return iterator ---\n10\n20\n30\n--- Many return values (16 limit) ---\n1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n--- Multi-return padding ---\nonly_one null null\nDONE')"

run_test "functions" "$SCRIPTS/functions.cdo" \
    "$(printf 'hello from function\n7\n30\n0\n1\n8\n3\n7\n12\n5\n8')"

run_test "inline_functions" "$SCRIPTS/inline_functions.cdo" \
    "$(printf '42\n3\n42\n42\n43\n42\n43\n43 44 45')"

run_test "closures" "$SCRIPTS/closures.cdo" \
    "$(printf '1\n2\n3\n1\n2\n2\n3\n3\n1\n2\n1\n3\n2')"

run_test "strings" "$SCRIPTS/strings.cdo" \
    "$(printf 'hello world\n5\nstring\nnumber\nbool\nnull\n42\n3.1400000000000001\ntrue\ntrue\nfalse\ntrue\n5\nworld\nhello world\nHELLO WORLD\nhi\nabc\ndef\nababab\n6\na\nb\nc\ncount: 99')"

run_test "template_strings" "$SCRIPTS/template_strings.cdo" \
    "$(printf 'hello\nhi world!\nworld\n3 = three\na42b43c\nbool=true null=null\nprepost\nauth=user:pass')"

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
    "$(printf '10\n20\n50\n2\n6\n2\n4\n4\n5\n1\n5\n1\n1\n2\n2\n3\n1\n3\n100\n7\n21')"

run_test "eval" "$SCRIPTS/eval.cdo" \
    "$(printf '6\n50\nfrom_sandbox\n10\ncontained\n42')"

run_test "include" "$SCRIPTS/include.cdo" \
    "$(printf 'Hello, World!\nHello, Cando!\ntrue\n1\n3\n4\nHello, Cache!')"

run_test "include_16" "$SCRIPTS/include_16.cdo" \
    "$(printf '1\n8\n16\ntrue\ntrue\ntrue')"

run_test "include_ext" "$SCRIPTS/include_ext.cdo" \
    "$(printf 'Cando\n1\ntrue\nalpha\nbeta\nalice\nbob\neve\nLA\nHello, Ext!\nMutated\nCando\n1\nalpha\ntrue')"

run_test "lib_csv" "$SCRIPTS/lib_csv.cdo" \
    "$(printf '2\nalice\n30\nNYC\nbob\nLA\n2\na\n2\nv1\nv2\nhello, world\nshe said "hi"\nalice\n25\n30\nbob')"

run_test "lib_yaml" "$SCRIPTS/lib_yaml.cdo" \
    "$(printf '42\n3.5\ntrue\nfalse\nnull\nnull\ntrue\nfalse\nhello\n42\nAlice\n30\ntrue\none\n2\ntrue\nnull\nlocalhost\n8080\ndebug\nverbose\nalice\n30\nbob\n25\n1\n3\n1\nhello\ntrue\n12\n14\nvalue\na\nb\ntrue\n3\n5\n5\n7\n6\nCando\n1\na\nb\ntrue\n3\n3\nflow_error_caught')"

run_test "threads" "$SCRIPTS/threads.cdo" \
    "$(printf '42\n10\n20\ntrue\nsleep_ok\nid_ok\n99\ntrue\nnull\n7\n1\n2\n3\ndone\nerror\nbad\ntrue\ntrue\n77\n88\ncaught_err\nalready\nfalse')"

run_test "uncaught_thread_error" "$SCRIPTS/uncaught_thread_error.cdo" \
    "$(printf 'script done\ncando: uncaught error in thread: boom from worker')"

run_test "caught_thread_error" "$SCRIPTS/caught_thread_error.cdo" \
    "$(printf 'caught: handled error\nthen-catch: handled via catch\nscript done')"

run_test "thread_error_observed" "$SCRIPTS/thread_error_observed.cdo" \
    "$(printf 'got: looked at via thread.error\nscript done')"

run_test "lib_os" "$SCRIPTS/lib_os.cdo" \
    "$(printf 'os.name: %s\nos_time_ok\nos_clock_ok\nPATH_ok\nCANDO_TEST: Hello' "$PLATFORM_OS_NAME")"

run_test "lib_datetime" "$SCRIPTS/lib_datetime.cdo" \
    "$(printf 'now_ok\nFormatted: 2023-10-27\nparse_ok')"

run_test "lib_array" "$SCRIPTS/lib_array.cdo" \
    "$(printf '3\n4\n4\n3\n2\n4\n6\n4\n6\n6')"

run_test "metamethods" "$SCRIPTS/metamethods.cdo" \
    "$(printf '10\n20\nhello\n99\n10\nfrom_a\nproto\nVec3\nnumber\nstring\nnull\nbool\n3\n4\nstring\nAnimal\nRex says hello\nRex\nPoint\n5\n7\n49\n27\n3\n11\n3\noverridden\nbase greet\n4\n6\n7\n15\n6\n20\n5\n5\n1\n1\n8\n9\n-5\n3\ntrue\nfalse\ntrue\ntrue\nfalse\ntrue\ntrue\ntrue\ntrue\n35\nVec(7,8)\nDog\nRex says hello\nRex says hello (woof, labrador)\nset:foo\nlooked_up:anything\nlooked_up:other\nbase_value\nfallback:unknown\nDynamicType\nliteral-form')"

run_test "meta_call" "$SCRIPTS/meta_call.cdo" \
    "$(printf '15\n42\nT:6\nping\n14\n1\n2\n3\n3\nwrap:99')"

run_test "lib_object" "$SCRIPTS/lib_object.cdo" \
    "$(printf '1\n99\n2\n1\n2\n3\n10\n99\n30\n20\nalice\nbob\nhello\nc\na\nb\n3\n1\n2\nfalse\ntrue\nfalse')"

run_test "inspect" "$SCRIPTS/inspect.cdo" \
    "$(printf 'null\ntrue\nfalse\n0\n42\n-7\n"hi"\n"a\\nb"\n[]\n[\n  1,\n  2,\n  3\n]\n[\n  1,\n  "two",\n  null\n]\n{}\n{\n  a: 1\n}\n{\n  a: 1,\n  b: 2\n}\n{\n  "with space": 1\n}\n{\n  list: [\n    1,\n    2,\n    [\n      3,\n      4\n    ]\n  ]\n}\n{\n  a: {\n    b: {\n      c: 1\n    }\n  }\n}\n{\n  a: {...}\n}\n{\n  a: {\n    b: {...}\n  }\n}\n{\n  a: {\n    b: {\n      c: 1\n    }\n  }\n}\n[\n  [...]\n]\n[\n  [\n    [...]\n  ]\n]\n[\n  1,\n  <circular>\n]\n{\n  x: 1,\n  self: <circular>\n}\n[\n  [\n    9\n  ],\n  [\n    9\n  ]\n]')"

run_test "lib_meta" "$SCRIPTS/lib_meta.cdo" \
    "$(printf 'object\nhttp_response\nhttp_request\nhttp_server\nhttp_client_response\nobject\nobject\nobject\nthread\nHI!\nYES!\n10\n42\ntrue\ndone\nobject\ngreeter\nhi, world')"

run_test "lib_http" "$SCRIPTS/lib_http.cdo" \
    "$(printf '200\nok-1\nbasic\nhello world\ndelayed\nhi alice')"

run_test "lib_socket" "$SCRIPTS/lib_socket.cdo" \
    "$(printf 'after listen\nping\necho:first\necho:second\nhi\nhi\ndone')"

run_test "lib_secure_socket" "$SCRIPTS/lib_secure_socket.cdo" \
    "$(printf 'after listen\ntls-echo:hello\ntrue\ntrue\ndone')"

run_test "lib_stream" "$SCRIPTS/lib_stream.cdo" \
    "$(printf 'memory\nhello, world\n'\'''\''\nfalse\n1600\n1600\n1600\nfinal\n'\'''\''\ntrue\nfile\nalpha-bravo\n'\'''\''\nthrew\n20\npiped through memory\n29\nCanDo streams compose nicely.\nfrom thread\nchannel\nhello-from-channel\ntcp\nECHO:hello\nfile-piped-as-response\npayload-from-http\n0\nspawned-output\n800\nnull\nstreamed-from-server\nHELLO, WORLD\ntransform\nPIPED THROUGH TRANSFORM\nok')"

run_test "lib_crypto" "$SCRIPTS/lib_crypto.cdo" \
    "$(printf 'md5_ok\nsha256_ok\naGVsbG8gd29ybGQ=\nhello world')"

run_test "lib_sys" "$SCRIPTS/lib_sys.cdo" \
    "$(printf 'pid_ok\nppid_ok')"

run_test "lib_enhance" "$SCRIPTS/lib_enhance.cdo" \
    "$(printf "math.log10(100):  2\nmath.exp(0):  1\nstartsWith('hello'):  true\nendsWith('world'):  true\nreplace('world', 'cando'): hello cando\nformat: Hello Alice, age 30")"

run_test "break_continue" "$SCRIPTS/break_continue.cdo" \
    "$(printf '0\n1\n2\n1\n2\n4\n5\n10\n20\n10\n20\n40\n50\n0\n1\n0\n1\n2\n0\n1\n3\n4\n0\n10\n20\ndone')"

run_test "new_syntax" "$SCRIPTS/new_syntax.cdo" \
    "$(printf 'yes\nno\na\nb\nc\nbig\n2 4\n3 4 5\n\n1 2 3 4 5\n99\nnull\nnull\n99\n99\nnull\nnull\nnull\n0\nfallback\nfalse\nfirst')"

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
