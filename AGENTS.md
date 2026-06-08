# PASTA / SVF Conditional Andersen — Agent Context

> Last updated: 2026-06-08

## Project Structure

- **Build system**: CMake, LLVM 17, Z3 4.15.4
- **WPA binary**: `~/PASTA/SVF/build/bin/wpa`
- **extapi.bc**: `~/PASTA/SVF/build/lib/extapi.bc`
- **Key source dirs**:
  - `SVF/svf/include/WPA/` — headers
  - `SVF/svf/lib/WPA/` — implementations
  - `SVF/svf/include/Util/` — PathCond, FastGuard
- **Benchmarks**:
  - `~/PASTA/benchmark/bc/jq.bc`
  - `~/TASA/benchmarks/BCs/file.bc`
  - `~/TASA/benchmarks/BCs/git.bc` (13MB)

## Key Classes

| Class | File | Role |
|-------|------|------|
| `SingleTrackCondAndersen` | `SingleTrackCondAndersen.h/cpp` | Lazy-sync single-track conditional Andersen. Inherits `ConditionalAndersenWaveDiff`. |
| `ConditionalAndersenWaveDiff` | `ConditionalAndersenWaveDiff.h/cpp` | Conditional propagation with wave diff. Core of guard propagation. |
| `PathCond` | `PathCond.h` | AST: True, False, Atom, And, Or. `depth()`, `isPureAndChain()`, `getAnd()`, `getOr()`. |
| `FastGuard` | `FastGuard.cpp` | DNF-based SAT checking backend. |

## Fixes Applied (Non-Monotonic Alias Precision)

### Root Cause
When `useDepthLimit` is enabled, `processCopy`/`processGep` only checked `cond->isPureAndChain()` to decide whether to retain current length. For mixed structures (Or-trees), `isPureAndChain()` returned `false`, causing `applyLimits()` to be called. In depth-limit mode, `applyLimits` collapses **any non-pure-And-chain** directly to `True`. This `True` guard then erased the concrete entry via `orMergeCondPts`. Later paths re-added the object with a different guard, flipping alias results from NoAlias (k=3) to MayAlias (k=5).

### Fix 1: Remove `isPureAndChain()` restriction
In `processCopy`/`processGep`, when depth would exceed `kLimit`, **retain current length for ALL structures** (not just pure And-chains). This prevents mixed Or-trees from being collapsed to `True`.

### Fix 2: Skip `True` guard propagation
In `processCopy`/`processGep` diff/fallback paths and `!guard->isTrue()` backfill paths: if the resulting guard is `True`, `continue` without calling `orMergeCondPts`. This prevents `True` from erasing concrete entries.

### Fix 3: Harden `orMergeCondPts` against `True` guards
When `guard->isTrue()` but the existing entry is concrete (non-True), **keep the existing guard** instead of erasing.

### Fix 4: Fixed Or-limit (`orLimit = 3`)
Or-merges make guards **looser** (more satisfiable). If the Or-limit grew with `k`, larger `k` could paradoxically produce *looser* guards than smaller `k`, breaking monotonicity. By fixing the Or-limit independently of `k`:
- **And-chains** can grow with `k` → more precise.
- **Or-trees** stay bounded → no extra looseness at larger `k`.

```cpp
// In orMergeCondPts
const u32_t orLimit = 3;
if (useDepthLimit &&
    merged->depth() > orLimit &&
    merged->depth() > it->second->depth())
{
    conjCappedGuards.insert(it->second);
    return false; // skip Or-merge, freeze guard
}
```

### Fix 5: Cap-marking mechanism (`conjCappedGuards`)
When a guard reaches the depth limit during `processCopy`/`processGep`, it is marked as "capped" in `conjCappedGuards`. Once capped:
- **And-operations** (`getAnd`) are skipped — the guard is frozen at its current length.
- **Or-operations** are bounded by the fixed `orLimit` (see Fix 4).

```cpp
if (isConjCapped(cond))
    newCond = cond; // already capped, do not append new literals
else if (useDepthLimit &&
         (std::max(cond->depth(), guard->depth()) + 1 > static_cast<u32_t>(kLimit)))
{
    newCond = cond;
    conjCappedGuards.insert(cond);
}
```

## Verification Results

### file.bc (SAT-enabled sampling)

| Metric | k=3 | k=5 | Monotonic? |
|--------|-----|-----|------------|
| refinedPtrs (1000 sample) | 163 | **205** | ✅ |
| totalRefined (1000 sample) | 1226 | **1312** | ✅ |
| refinedPtrs (2000 sample) | 316 | **392** | ✅ |
| totalRefined (2000 sample) | 2430 | **2600** | ✅ |

### Performance

| Benchmark | k=3 | k=5 |
|-----------|-----|-----|
| file.bc (no sampling) | ~2s | ~3s |
| file.bc (SAT 1000) | ~2s | ~3s |
| jq.bc (no sampling) | ~66s | ~89s |

## Batch Script

`~/PASTA/run_cond_andersen.sh` — runs k=1,3,5 for given bc files.

```bash
cd ~/PASTA
./run_cond_andersen.sh 10000 ./benchmark/bc/git.bc ./benchmark/bc/file.bc
```

- First arg: sample size (positive integer)
- Remaining args: bc file paths
- Output: `~/PASTA/benchmark/result/<basename>-cond-ander-wave-k<x]-depth-mcs-edgeguard-single-eager-precisionSample<nnn>-<yymmdd>.txt`

## Default Options

| Option | Default Value |
|--------|---------------|
| `kLimit` | `-1` (unlimited) |
| `useDepthLimit` | `true` |
| `mergeCondSCC` | `false` (can override via `-cond-ander-merge-cond-scc`) |
| `eagerSat` | `false` |
| `useFastGuard` | `true` |

## Key Invariants

- `condPtsMap` is a **subset** of bitvector pts (enforced by `ensureNodeSynced` and `finalize()` cleanup).
- `False` guards are used to record eagerSat filtering instead of erasing entries.
- `applyLimits` in depth-based mode: pure And-chain → truncate to last `kLimit` literals; mixed structure → collapse to `True`.
- `getOr` has a safety cap: if child depth > 100, returns `CappedTrue` (treated as `True`).
