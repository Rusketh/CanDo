#!/usr/bin/env bash
# run_bench.sh -- run every CanDo benchmark and print wall-clock timings.
#
# Usage: run_bench.sh <cando-binary> <script.cdo>...
#
# Output is purely informational; this script never sets a non-zero exit
# status on slow times.  A script that crashes or returns non-zero is
# reported as FAIL.  See docs/jit-plan.md for the role these benchmarks
# play in the JIT roadmap.

set -uo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <cando-binary> <script.cdo>..." >&2
    exit 2
fi

CANDO="$1"; shift

for script in "$@"; do
    name=$(basename "$script" .cdo)
    start=$(date +%s.%N)
    if ! "$CANDO" "$script" >/dev/null 2>&1; then
        printf "  %-16s FAIL\n" "$name"
        continue
    fi
    end=$(date +%s.%N)
    elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.3f", e - s }')
    printf "  %-16s %s s\n" "$name" "$elapsed"
done
