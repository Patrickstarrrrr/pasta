#!/usr/bin/env bash
# run_sample.sh -- Randomly sample k demo cases for regression testing.
# Much faster than run_all.sh when you just want a quick smoke test.
#
# Usage: ./run_sample.sh [k]
#   k = number of cases to sample (default: 5)

set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WPA="$ROOT/SVF/build/bin/wpa"
EXTAPI="$ROOT/SVF/build/lib/extapi.bc"
CODE="$ROOT/demo/code"
BC="$ROOT/demo/bc"
RESULT="$ROOT/demo/result"

K="${1:-5}"
mkdir -p "$BC" "$RESULT"

# Collect all .c files and shuffle
all_cases=($(ls "$CODE"/*.c | sort -R))
sample=(${all_cases[@]:0:K})

echo "=== Sampling $K of ${#all_cases[@]} demo cases ==="
for src in "${sample[@]}"; do
    name=$(basename "$src" .c)
    bc="$BC/$name.bc"
    out_a="$RESULT/$name.ander.txt"
    out_c="$RESULT/$name.cond.txt"

    echo "==> $name"
    clang -emit-llvm -c -O0 -g "$src" -o "$bc" 2>/dev/null

    # Count MayAlias only (no need to store all pairs)
    a_may=$("$WPA" -ander -print-aliases -stat=false -extapi="$EXTAPI" "$bc" 2>&1 | grep -cE "^MayAlias" || true)
    c_may=$("$WPA" -cond-ander -print-aliases -stat=false -extapi="$EXTAPI" "$bc" 2>&1 | grep -cE "^MayAlias" || true)

    refined=$(( a_may - c_may ))
    echo "    ander=$a_may  cond=$c_may  refined=$refined"
done

echo "=== Done ($K cases sampled) ==="
