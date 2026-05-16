#!/bin/bash
set -e
CLANG=$(which clang 2>/dev/null || echo /opt/homebrew/opt/llvm@17/bin/clang)
for f in /Users/jiayi/PASTA/demo2/*.c; do
    name=$(basename "$f" .c)
    echo "Compiling $name.c -> $name.bc"
    "$CLANG" -c -emit-llvm -g -O0 "$f" -o "/Users/jiayi/PASTA/demo2/$name.bc"
done
echo "All demos compiled."
