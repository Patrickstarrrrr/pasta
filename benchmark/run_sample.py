#!/usr/bin/env python3
"""
run_sample.py -- Sample n random alias pairs from real programs.
Avoids printing O(n^2) alias pairs by using reservoir sampling.

Strategy:
  1. Run Andersen with -print-aliases to collect all valid pointer IDs.
  2. Randomly sample N pairs of IDs.
  3. Run both Andersen and CondAnder (without -print-aliases) to get PTA results.
  4. Query alias() for the sampled pairs in both analyses and compare.

Usage:
    python3 run_sample.py <program.bc> [-n N]

Example:
    python3 run_sample.py bc/cjson.bc -n 1000
"""

import argparse
import subprocess
import random
import sys
import re
from pathlib import Path


def get_pointer_ids(bc_path, wpa_path, extapi_path):
    """Run -print-pts to extract all top-level pointer NodeIDs."""
    cmd = [wpa_path, "-ander", "-print-pts", "-stat=false",
           "-extapi=" + extapi_path, bc_path]
    result = subprocess.run(cmd, capture_output=True, text=True)
    ids = set()
    for line in result.stdout.splitlines() + result.stderr.splitlines():
        m = re.match(r"NodeID\s+(\d+)", line)
        if m:
            ids.add(int(m.group(1)))
    return sorted(ids)


def sample_pairs(ids, n):
    """Randomly sample n pairs of distinct IDs."""
    pairs = set()
    max_attempts = n * 10
    attempts = 0
    while len(pairs) < n and attempts < max_attempts:
        a, b = random.sample(ids, 2)
        pair = (min(a, b), max(a, b))
        pairs.add(pair)
        attempts += 1
    return list(pairs)


def run_pta(bc_path, wpa_path, extapi_path, mode):
    """Run PTA and return the output."""
    cmd = [wpa_path, mode, "-stat=false", "-extapi=" + extapi_path, bc_path]
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.stdout + result.stderr


def query_aliases(bc_path, wpa_path, extapi_path, mode, pairs):
    """Query alias() for specific pairs by injecting a custom pass.
    Since wpa doesn't support pair-by-pair querying, we use a workaround:
    run with -print-aliases and grep for our sampled pairs."""
    # For now, we run full -print-aliases and filter.
    # A more efficient approach would be to modify wpa to accept a pair list.
    cmd = [wpa_path, mode, "-print-aliases", "-stat=false",
           "-extapi=" + extapi_path, bc_path]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)

    results = {}
    pair_set = set(pairs)
    count = 0
    for line in proc.stdout:
        if not line.startswith("MayAlias") and not line.startswith("NoAlias"):
            continue
        count += 1
        # Parse "MayAlias var123[...] -- var456[...]"
        m = re.match(r"(MayAlias|NoAlias) var(\d+)\[.*\] -- var(\d+)\[.*\]", line)
        if not m:
            continue
        result = m.group(1)
        a, b = int(m.group(2)), int(m.group(3))
        pair = (min(a, b), max(a, b))
        if pair in pair_set:
            results[pair] = result
            pair_set.discard(pair)
            if not pair_set:
                proc.terminate()
                break

    proc.wait()
    return results, count


def main():
    parser = argparse.ArgumentParser(description="Sample alias pairs from PTA")
    parser.add_argument("bc", help="Path to bitcode file")
    parser.add_argument("-n", type=int, default=1000,
                        help="Number of pairs to sample (default: 1000)")
    parser.add_argument("-extapi", default=None,
                        help="Path to extapi.bc")
    parser.add_argument("--wpa", default="SVF/build/bin/wpa",
                        help="Path to wpa binary (relative to repo root)")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent
    wpa_path = str(root / args.wpa)
    extapi_path = args.extapi or str(root / "SVF/build/lib/extapi.bc")
    bc_path = str(Path(args.bc).resolve())

    if not Path(wpa_path).exists():
        print(f"Error: wpa not found at {wpa_path}", file=sys.stderr)
        sys.exit(1)

    print(f"=== Step 1: Collecting pointer IDs from {Path(bc_path).name} ===")
    ids = get_pointer_ids(bc_path, wpa_path, extapi_path)
    print(f"  Found {len(ids)} top-level pointers")
    if len(ids) < 2:
        print("Error: Not enough pointers to sample", file=sys.stderr)
        sys.exit(1)

    print(f"=== Step 2: Sampling {args.n} random pairs ===")
    pairs = sample_pairs(ids, args.n)
    print(f"  Sampled {len(pairs)} unique pairs")

    print(f"=== Step 3: Querying Andersen ===")
    results_a, total_a = query_aliases(bc_path, wpa_path, extapi_path, "-ander", pairs)
    print(f"  Scanned ~{total_a} pairs, found {len(results_a)} sampled pairs")

    print(f"=== Step 4: Querying Conditional Andersen ===")
    results_c, total_c = query_aliases(bc_path, wpa_path, extapi_path, "-cond-ander", pairs)
    print(f"  Scanned ~{total_c} pairs, found {len(results_c)} sampled pairs")

    # Compare
    all_pairs = set(pairs)
    common = set(results_a.keys()) & set(results_c.keys())
    agree = sum(1 for p in common if results_a[p] == results_c[p])
    disagree = [p for p in common if results_a[p] != results_c[p]]

    may_a = sum(1 for p in common if results_a[p] == "MayAlias")
    may_c = sum(1 for p in common if results_c[p] == "MayAlias")

    print(f"\n=== Results (on {len(common)} common pairs) ===")
    print(f"Andersen MayAlias:  {may_a}/{len(common)} ({100*may_a/len(common):.1f}%)")
    print(f"CondAnder MayAlias: {may_c}/{len(common)} ({100*may_c/len(common):.1f}%)")
    print(f"Agreement: {agree}/{len(common)} ({100*agree/len(common):.1f}%)")

    if len(common) < len(all_pairs):
        print(f"Note: {len(all_pairs) - len(common)} sampled pairs were not found in output")

    if disagree:
        print(f"\nDisagreements ({len(disagree)} pairs):")
        for a, b in sorted(disagree)[:20]:
            print(f"  var{a} -- var{b}:  A={results_a[(a,b)]}  C={results_c[(a,b)]}")
        if len(disagree) > 20:
            print(f"  ... and {len(disagree) - 20} more")
    else:
        print("\nNo disagreements on sampled pairs.")


if __name__ == "__main__":
    main()
