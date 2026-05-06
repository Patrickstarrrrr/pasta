#!/usr/bin/env bash
# benchmark/run_benchmark.sh -- Compare Andersen vs Conditional Andersen
# Phase 1: pure analysis time (no -print-aliases)
# Phase 2: MayAlias count for cjson only (with -print-aliases)

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPA="$ROOT/SVF/build/bin/wpa"
EXTAPI="$ROOT/SVF/build/lib/extapi.bc"
BC="$ROOT/benchmark/bc"
RESULT="$ROOT/benchmark/result"

mkdir -p "$BC" "$RESULT"

OPENCODE_BC="/Users/jiayi/PASTA_opencode/benchmark/bc"

SUMMARY="$RESULT/SUMMARY.md"
{
    echo "# Benchmark: Andersen vs Conditional Andersen"
    echo
    echo "Phase 1: Pure analysis time (no alias printing)."
    echo "Phase 2: MayAlias count for cjson (with -print-aliases)."
    echo
    echo "| Program | IR lines | -ander time | -cond-ander time | slowdown |"
    echo "|---------|----------|-------------|------------------|----------|"
} > "$SUMMARY"

count_may () {
    grep -cE "^MayAlias" "$1" || true
}

run_time () {
    local prog="$1"
    local timeout_s="${2:-120}"
    local src_bc="$BC/$prog.bc"

    if [ ! -f "$src_bc" ] && [ -f "$OPENCODE_BC/$prog.bc" ]; then
        cp "$OPENCODE_BC/$prog.bc" "$src_bc"
    fi
    if [ ! -f "$src_bc" ]; then
        echo "SKIP $prog (no bitcode)"
        return
    fi

    echo "==> $prog (time only, timeout=${timeout_s}s)"

    local t0=$(date +%s)
    timeout "$timeout_s" "$WPA" -ander -stat=false -extapi="$EXTAPI" "$src_bc" > /dev/null 2>&1 || true
    local secs_a=$(($(date +%s) - t0))

    local t1=$(date +%s)
    timeout "$timeout_s" "$WPA" -cond-ander -stat=false -extapi="$EXTAPI" "$src_bc" > /dev/null 2>&1 || true
    local secs_c=$(($(date +%s) - t1))

    local slowdown="N/A"
    if [ "$secs_a" -gt 0 ]; then
        slowdown=$(awk -v c=$secs_c -v a=$secs_a 'BEGIN{printf "%.1fx", c/a}')
    fi

    local lines=0
    if [ -f "$OPENCODE_BC/$prog.ll" ]; then
        lines=$(wc -l < "$OPENCODE_BC/$prog.ll" | tr -d ' ')
    fi

    echo "    ander=${secs_a}s  cond=${secs_c}s  slowdown=$slowdown"
    echo "| \`$prog\` | $lines | ${secs_a}s | ${secs_c}s | $slowdown |" >> "$SUMMARY"
}

run_count () {
    local prog="$1"
    local timeout_s="${2:-300}"
    local src_bc="$BC/$prog.bc"

    echo "==> $prog (MayAlias count, timeout=${timeout_s}s)"

    local out_a="$RESULT/$prog.ander.txt"
    local out_c="$RESULT/$prog.cond.txt"

    local t0=$(date +%s)
    timeout "$timeout_s" "$WPA" -ander -print-aliases -stat=false -extapi="$EXTAPI" "$src_bc" > "$out_a" 2>&1 || true
    local secs_a=$(($(date +%s) - t0))

    local t1=$(date +%s)
    timeout "$timeout_s" "$WPA" -cond-ander -print-aliases -stat=false -extapi="$EXTAPI" "$src_bc" > "$out_c" 2>&1 || true
    local secs_c=$(($(date +%s) - t1))

    local a_may=$(count_may "$out_a")
    local c_may=$(count_may "$out_c")
    local refined=$(( a_may - c_may ))

    echo "    ander=${secs_a}s ($a_may MayAlias)  cond=${secs_c}s ($c_may MayAlias)  refined=$refined"
    {
        echo
        echo "## MayAlias count for \`$prog\`"
        echo
        echo "| -ander time | -cond-ander time | ander MayAlias | cond MayAlias | refined |"
        echo "|-------------|------------------|----------------|---------------|---------|"
        echo "| ${secs_a}s | ${secs_c}s | $a_may | $c_may | $refined |"
    } >> "$SUMMARY"
}

# --- Phase 1: time only ---
run_time cjson   120
run_time bzip2   120
run_time zlib    120

# --- Phase 2: MayAlias count for cjson ---
run_count cjson  300

echo
echo "Done. See $SUMMARY"
