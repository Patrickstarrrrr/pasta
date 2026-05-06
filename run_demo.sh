#!/bin/bash
set -e

BC="test_demo.bc"
EXTAPI="SVF/build/lib/extapi.bc"
WPA="SVF/build/bin/wpa"

echo "========================================"
echo "  Demo: Andersen vs Conditional Andersen"
echo "========================================"
echo ""
echo "Test program: test_demo.c"
echo ""
echo "--- Source code ---"
head -n 40 test_demo.c
echo ""
echo "========================================"
echo ""

echo "[1] Traditional Andersen (-ander)"
echo "----------------------------------------"
$WPA -ander -print-aliases -extapi=$EXTAPI $BC 2>&1 | grep -E "o2.*o3|o3.*o2" | head -5
echo ""

echo "[2] Conditional Andersen (-cond-ander)"
echo "----------------------------------------"
$WPA -cond-ander -print-aliases -extapi=$EXTAPI $BC 2>&1 | grep -E "o2.*o3|o3.*o2" | head -5
echo ""

echo "========================================"
echo "[3] Conditional Points-To (CondAnder only)"
echo "----------------------------------------"
$WPA -cond-ander -print-pts -extapi=$EXTAPI $BC 2>&1 | grep -E "Node.*\[o2\]|Node.*\[o3\]|Node.*\[p\]:" | head -10
echo ""
echo "========================================"
