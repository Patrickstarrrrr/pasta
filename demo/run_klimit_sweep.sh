#!/usr/bin/env bash
# run_klimit_sweep.sh -- Sweep k-limit values and collect time/refined stats
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPA="$ROOT/SVF/build/bin/wpa"
EXTAPI="$ROOT/SVF/build/lib/extapi.bc"
CODE="$ROOT/demo/code"
BC="$ROOT/demo/bc"
RESULT="$ROOT/demo/result_klimit"

mkdir -p "$BC" "$RESULT"

KVALUES=(0 1 2 3 4 5 99)

count_may () {
    grep -cE "^MayAlias" "$1" || true
}

# CSV header
echo "case,k_limit,ander_may,cond_may,refined,time_ms" > "$RESULT/stats.csv"

for src in "$CODE"/*.c; do
    name=$(basename "$src" .c)
    bc="$BC/$name.bc"
    clang -emit-llvm -c -O0 -g "$src" -o "$bc" 2>/dev/null || true

    echo "==> $name"

    # Baseline: vanilla Andersen (run once per case)
    out_a="$RESULT/$name.ander.txt"
    "$WPA" -ander -print-aliases -stat=false -extapi="$EXTAPI" "$bc" > "$out_a" 2>&1 || true
    a_may=$(count_may "$out_a")

    for k in "${KVALUES[@]}"; do
        out_c="$RESULT/$name.k${k}.txt"

        # Time the conditional Andersen run
        start_ms=$(perl -MTime::HiRes=time -e 'printf "%.0f\n", time*1000')
        "$WPA" -cond-ander -cond-ander-k="$k" -print-aliases -stat=false -extapi="$EXTAPI" "$bc" > "$out_c" 2>&1 || true
        end_ms=$(perl -MTime::HiRes=time -e 'printf "%.0f\n", time*1000')
        elapsed=$(( end_ms - start_ms ))

        c_may=$(count_may "$out_c")
        refined=$(( a_may - c_may ))

        echo "    k=$k  ander=$a_may  cond=$c_may  refined=$refined  time=${elapsed}ms"
        echo "$name,$k,$a_may,$c_may,$refined,$elapsed" >> "$RESULT/stats.csv"
    done
done

echo
echo "Done. Results in $RESULT/stats.csv"
