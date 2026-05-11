#!/usr/bin/env bash
# run_error_tests.sh -- Drive every script under tests/scripts/errors/ through
# ./cando and record stdout, stderr, and exit status.  This is a deliberately
# untyped run: the suite exists to evaluate the *quality* of error reporting,
# not to assert a fixed expected output.  All output is written to
# tests/error_reports/report.txt.
#
# Usage: tests/integration/run_error_tests.sh [path/to/cando]

set -u
CANDO="${1:-./cando}"
ROOT="tests/scripts/errors"
OUT="tests/error_reports/report.txt"

mkdir -p "$(dirname "$OUT")"
: > "$OUT"

declare -i total=0
declare -i with_error=0
declare -i silent_failures=0
declare -i exit_zero=0

for category in lex parse runtime; do
    cat_dir="$ROOT/$category"
    [[ -d "$cat_dir" ]] || continue
    {
        echo "============================================================"
        echo "CATEGORY: $category"
        echo "============================================================"
    } >> "$OUT"

    # shellcheck disable=SC2045
    for script in $(ls "$cat_dir"/*.cdo 2>/dev/null | sort); do
        total+=1
        name=$(basename "$script")

        out=$("$CANDO" "$script" 2>/tmp/cando_err.$$ </dev/null)
        rc=$?
        err=$(cat /tmp/cando_err.$$)
        rm -f /tmp/cando_err.$$

        {
            echo
            echo "------------------------------------------------------------"
            echo "TEST  : $category/$name"
            echo "EXIT  : $rc"
            echo "STDOUT:"
            if [[ -n "$out" ]]; then printf '  %s\n' "$out" | sed 's/^/| /'; else echo "  <empty>"; fi
            echo "STDERR:"
            if [[ -n "$err" ]]; then printf '  %s\n' "$err" | sed 's/^/| /'; else echo "  <empty>"; fi
        } >> "$OUT"

        if [[ $rc -eq 0 ]]; then
            exit_zero+=1
        fi
        if [[ -n "$err" ]]; then
            with_error+=1
        elif [[ $rc -ne 0 ]]; then
            silent_failures+=1
        fi
    done
done

{
    echo
    echo "============================================================"
    echo "SUMMARY"
    echo "============================================================"
    echo "total scripts        : $total"
    echo "produced stderr      : $with_error"
    echo "exited 0 (no error?) : $exit_zero"
    echo "non-zero but silent  : $silent_failures"
} >> "$OUT"

echo "report written to $OUT"
echo "total=$total stderr=$with_error exit_zero=$exit_zero silent_failures=$silent_failures"
