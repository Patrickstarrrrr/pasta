# PASTA / SVF Conditional Andersen — Agent Context

> Last updated: 2026-06-16

## Project Structure

- **Build system**: CMake, LLVM 17, Z3 4.15.4
- **WPA binary**: `~/PASTA/SVF/build/bin/wpa`
- **extapi.bc**: `~/PASTA/SVF/build/lib/extapi.bc`
- **Key source dirs**:
  - `SVF/svf/include/WPA/` — headers
  - `SVF/svf/lib/WPA/` — implementations
  - `SVF/svf/include/Util/` — PathCond, FastGuard
- **Benchmarks** (current suite in `~/PASTA/benchmark/bc/`):
  - `git.bc` (13MB)
  - `perl.bc` (14MB, 新增 — 替换 oversized ffmpeg)
  - `redis_nginx.bc` (15MB, 新增 — 替换 postgres)
  - `redis.bc` (8.7MB, 新增)
  - `vim.bc` (8.3MB)
  - `tmux.bc` (7.4MB)
  - `sqlite.bc` (6.7MB)
  - `nginx.bc` (6MB, 新增)
  - `jq.bc` (3.3MB)
  - `curl.bc` (1.5MB, 新增)
  - `~/TASA/benchmarks/BCs/file.bc`
  - `~/TASA/benchmarks/BCs/git.bc` (13MB)
- **Removed benchmarks**:
  - `ffmpeg.bc` (44MB) — oversized, plain Andersen runs out of memory
  - `postgres.bc` (16MB) — oversized and hits SVF `ObjTypeInference` error

## Key Classes

| Class | File | Role |
|-------|------|------|
| `SingleTrackCondAndersen` | `SingleTrackCondAndersen.h/cpp` | Lazy-sync single-track conditional Andersen. Inherits `ConditionalAndersenWaveDiff`. |
| `ConditionalAndersenWaveDiff` | `ConditionalAndersenWaveDiff.h/cpp` | Conditional propagation with wave diff. Core of guard propagation. |
| `PathCond` | `PathCond.h` | AST: True, False, Atom, And, Or. `depth()`, `isPureAndChain()`, `getAnd()`, `getOr()`. |
| `FastGuard` | `FastGuard.cpp` | DNF-based SAT checking backend. |

## Statistics Added

### Eager SAT Cut Counter (`numEagerSatCuts`)
Added a counter to track how many objects are filtered out by eager SAT checking during propagation.

**Files modified**:
- `SVF/svf/include/WPA/ConditionalAndersen.h` — added `mutable u32_t numEagerSatCuts`
- `SVF/svf/include/WPA/ConditionalAndersenWaveDiff.h` — added `mutable u32_t numEagerSatCuts`
- `SVF/svf/lib/WPA/ConditionalAndersen.cpp` — initialization, accumulation at 4 locations, output in stats
- `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp` — initialization, accumulation at 5 locations, output in stats
- `SVF/svf/lib/WPA/SingleTrackCondAndersen.cpp` — output in `SingleTrackCondAndersen Statistics`

**Output format**:
```
========== SingleTrackCondAndersen Statistics ==========
  analysisComplete:    true
  eagerSat:            true
  Z3 SAT checks:       79046041
  CondPtsMap nodes:    49274
  CondPtsMap entries:  45365905
  Eager SAT cuts:      128217    <-- 新增统计
  ...
```

**Cut locations** (two types):
1. **Single-object cut**: `processCopy`/`processGep` per-object propagation — `newCond` is unsat → skip
2. **Batch cut**: backfill path — `limitedGuard` is unsat → skip all objects in `pts` not yet in `condPtsMap`

### FlowSensitive Baseline Statistics
Added per-pointer precision sampling to `FlowSensitive` so it can be compared against the internal Andersen baseline.

**Files modified**:
- `SVF/svf/include/Util/Options.h` — added `FlowSensitivePrecisionSample` option
- `SVF/svf/lib/Util/Options.cpp` — registered `-fs-precision-sample` CLI option
- `SVF/svf/include/WPA/FlowSensitive.h` — added `precisionSampleSize`, `samplePrecisionGain()`, `sampleAliasPartnerReduction()`
- `SVF/svf/lib/WPA/FlowSensitive.cpp` — implemented sampling and invoked from `finalize()`

**Two sampling metrics** (both use the internal `AndersenWaveDiff` baseline):
1. **`samplePrecisionGain`**: for each sampled top-level pointer, compare `|pts_Andersen|` vs `|pts_FlowSensitive|`. Because Andersen collapses each SCC into a single representative points-to set, FlowSensitive's points-to sets are aggregated over the same Andersen SCC before comparison.
2. **`sampleAliasPartnerReduction`**: for each sampled top-level pointer, count how many Andersen alias partners are refined to `NoAlias` by FlowSensitive. Both analyzers use their own `alias()` method so SCC handling is consistent.

**Output format** (excerpt):
```
  [fs-samplePrecision] Done. sampled=100 totalAndersenSize=2689 totalFlowSensitiveSize=805 refined=25 ...
  [fs-samplePartners] Done. sampled=100 totalAndersenPartners=1208 totalRefined=710 refinedPtrs=37 ...
```

### Build Status
Successfully rebuilt after adding `numEagerSatCuts` and FlowSensitive statistics. All `wpa`/`saber` targets built successfully.

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

| Benchmark | k=3 | k=5 | Plain Andersen |
|-----------|-----|-----|----------------|
| file.bc (no sampling) | ~2s | ~3s | — |
| file.bc (SAT 1000) | ~2s | ~3s | — |
| jq.bc (no sampling) | ~66s | ~89s | — |
| perl.bc (no sampling) | TBD | TBD | ~11m |
| redis_nginx.bc (no sampling) | TBD | TBD | ~6m |

## Batch Scripts

### Conditional Andersen
`~/PASTA/run_cond_andersen.sh` — runs k=1,3,5 for given bc files.

```bash
cd ~/PASTA
./run_cond_andersen.sh 10000 ./benchmark/bc/git.bc ./benchmark/bc/file.bc
```

- First arg: sample size (positive integer)
- Remaining args: bc file paths
- Output: `~/PASTA/benchmark/result/<basename>-cond-ander-wave-k<x]-depth-mcs-edgeguard-single-eager-precisionSample<nnn>-<yymmdd>.txt`

### FlowSensitive Baseline
`~/PASTA/run_flow_sensitive.sh` — runs FlowSensitive once per bc file and samples top-level pointers to compare precision against Andersen.

```bash
cd ~/PASTA
./run_flow_sensitive.sh 1000 ./benchmark/bc/git.bc ./benchmark/bc/perl.bc
```

- First arg: sample size (positive integer)
- Remaining args: bc file paths
- Output: `~/PASTA/benchmark/result/<basename>-flow-sensitive-precisionSample<nnn>-<yymmdd>.txt`

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

## SPAS (Path-Sensitive Flow-Sensitive Baseline) — In Progress

We are implementing a SPAS-style (flow-, context-, and path-sensitive) pointer analysis on top of SVF as the path-sensitive baseline. The work is split into phases.

### Phase 0 — Guard / BDD Infrastructure (DONE)

- Built and installed **CUDD 3.0.0** at `~/PASTA/deps/cudd-install`.
- Added `FindCUDD.cmake` and wired CUDD into `SVF/CMakeLists.txt` as an optional dependency.
- Fixed a macro conflict between CUDD's `epsilon` parameter and `AE/Core/NumericValue.h`'s `#define epsilon` by turning it into a `constexpr double`.
- Added `SVF/svf/include/Util/Guard.h` and `SVF/svf/lib/Util/Guard.cpp`:
  - BDD-backed guard abstraction (`Guard`) when `SVF_ENABLE_SPAS` is defined.
  - Supports path atoms (`Guard::PathAtom`) and context atoms (`Guard::ContextAtom`).
  - Operations: `&`, `|`, `!`, `isTrue()`, `isFalse()`, `isSat()`, `implies()`, hash.
  - Falls back to `PathCond` if CUDD is unavailable.

### Parameterized Control

All SVF data-structure changes are gated by the CMake option `SVF_ENABLE_SPAS` (default `OFF`):

| Component | Default build (SPAS OFF) | SPAS build (SPAS ON) |
|---|---|---|
| `MemRegion::Condition` | `bool` | `const Guard*` |
| `MSSAMU`/`MSSACHI`/`MSSAPHI` default condition | `true` | `Guard::getTruePtr()` |
| `IndirectSVFGEdge` | no extra field | `Guard` field |
| `SVFG::add*IndirectVFEdge` | original signature | optional guard parameter |
| `PathSensitiveFlowSensitive` | not compiled | compiled; `-psfspta` registered |
| `PointerAnalysis::PathS_FSSPARSE_WPA` | not defined | defined |

This guarantees that a normal SVF build is byte-for-byte identical to the pre-SPAS state (aside from making `FlowSensitive::processSVFGNode` virtual, which only adds one vtable slot).

### Phase 1 — Intraprocedural Path-Sensitive Flow-Sensitive (DONE)

- `MemRegion::Condition` is `const Guard*` (SPAS only).
- `MSSAMU`/`MSSACHI`/`MSSAPHI` default condition is `Guard::getTruePtr()` (SPAS only).
- Added a `Guard` field to `IndirectSVFGEdge` and updated `SVFG::add*IndirectVFEdge(...)` to accept/merge guards (SPAS only).
- Added `SVF/svf/include/MemoryModel/CondPointsToDS.h` — conditional data-flow points-to store `(loc, var, guard) -> PointsTo`.
- Added `PathSensitiveFlowSensitive` class (`WPA/PathSensitiveFlowSensitive.h/.cpp`):
  - Registered as `-psfspta` in `wpa`.
  - New `PointerAnalysis::PTATY` value `PathS_FSSPARSE_WPA`.
  - Overrides `processLoad`/`processStore` to use `CondDFPTData`.
  - Overrides `propAlongIndirectEdge` to propagate conditional facts guarded by the SVFG edge guard.
  - Implements path-sensitive weak (MAY) updates and conditional strong (MUST) updates for singleton targets.
- Fixed `MemSSA::getNodeGuard` / `PathSensitiveFlowSensitive::getNodeGuard` to compute the guard of a statement from its basic-block entry (branch conditions live on inter-block ICFG edges, not on intra-block statement edges).
- Added `run_spas.sh` for batch evaluation.

### Phase 2 — Context Sensitivity (DONE)

- `Guard` now maintains a BDD cube of all context-atom variables and supports:
  - `withoutContextAtoms()` / `isContextIndependent()` for context stripping.
- `SVFG::addInterIndirectVFCallEdge` / `addInterIndirectVFRetEdge` tag interprocedural indirect edges with a `ContextAtom` guard derived from the `CallSiteID`.
- `DirectSVFGEdge` (SPAS only) now carries a `Guard` field; `CallDirSVFGEdge` / `RetDirSVFGEdge` are tagged with a context atom for their call site.
- `PathSensitiveFlowSensitive::propAlongDirectEdge` propagates actual/formal parameters and returns under the call-site context guard, separating top-level values by call site.
- `PathSensitiveFlowSensitive` stores top-level pointer facts in `CondDFPTData` alongside address-taken facts; `ptD` is kept only as an unconditional fallback for SVF internals.
- `PathSensitiveFlowSensitive` maintains an active-context guard per function (`funContextGuard`) and ANDs it into store guards, so address-taken effects inside a callee are tagged with the calling contexts that may reach it.

### Phase 3 — Level-by-Level Refinement & Guard-Aware Strong Updates (DONE)

- Added guard-size metrics to `Guard`: `nodeCount()` and `supportSize()`.
- Added SPAS-specific CLI options:
  - `-psfs-k <int>` — guard support-size limit (`-1` = unlimited, `0` = cap all non-trivial guards).
  - `-psfs-use-depth-limit` — enable guard capping during propagation.
  - `-psfs-refine` — run a level-by-level refinement loop for `k = 1 .. psfs-k`.
- `PathSensitiveFlowSensitive::solveConstraints` overrides the base solver to support the refinement loop: each level clears the conditional store and re-solves with the current `k`; the unconditional fallback `ptD` is kept monotonic so call-graph refinement remains sound.
- All guard combinations in `processCopy`/`processGep`/`processPhi`/`processLoad`/`processStore`/`propAlongDirectEdge`/`propAlongIndirectEdge` are passed through `capGuard`, which weakens guards exceeding `k` to `True`.
- `processStore` now performs **guard-aware strong updates**: a store target is treated as a singleton (and therefore strong-updatable under that guard) when the conditional points-to set of the destination pointer is a single object under the incoming guard, rather than relying on the unconditional singleton check.

### SPAS CLI Options

| Option | Default | Meaning |
|--------|---------|---------|
| `-psfs-k` | `-1` | Guard support-size limit (`-1` = unlimited) |
| `-psfs-use-depth-limit` | `false` | Weaken guards whose support size exceeds `-psfs-k` to `True` |
| `-psfs-refine` | `false` | Iterate the analysis for `k = 1 .. psfs-k` |

### Micro-Benchmarks

- `demo2/05_psfs_test.c` — branch-local load refinement.
- `demo2/06_ctx_test.c` — context-sensitive memory facts through a global pointer.
- `demo2/07_ctx_return.c` — context-sensitive top-level return values (`id(&a)` vs `id(&b)`).

### Next Steps

1. Expand precision evaluation against `FlowSensitive` on the benchmark suite.
2. Implement demand-driven alias-query refinement (split may-alias candidates by additional path atoms).
3. Build per-function MOD/USE summaries to avoid re-analyzing callees at every refinement level.

### Build / Run

```bash
# Default build (SPAS disabled; no CUDD dependency; no impact on existing analyses)
cd ~/PASTA/SVF/build
cmake .. -DZ3_HOME=/opt/homebrew/Cellar/z3/4.15.4 \
         -DLLVM_DIR=/opt/homebrew/opt/llvm@17/lib/cmake/llvm
make -j$(sysctl -n hw.ncpu)

# SPAS-enabled build (requires CUDD installed at ~/PASTA/deps/cudd-install)
cd ~/PASTA/SVF/build
cmake .. -DSVF_ENABLE_SPAS=ON -DCUDD_HOME=~/PASTA/deps/cudd-install \
         -DZ3_HOME=/opt/homebrew/Cellar/z3/4.15.4 \
         -DLLVM_DIR=/opt/homebrew/opt/llvm@17/lib/cmake/llvm
make -j$(sysctl -n hw.ncpu)

# Run the new analysis (only available when built with -DSVF_ENABLE_SPAS=ON)
~/PASTA/SVF/build/bin/wpa -psfspta -stat=true -extapi=~/PASTA/SVF/build/lib/extapi.bc <bc>

# Level-by-level refinement example (k = 1..3)
~/PASTA/SVF/build/bin/wpa -psfspta -psfs-use-depth-limit -psfs-k=3 -psfs-refine \
    -stat=true -extapi=~/PASTA/SVF/build/lib/extapi.bc <bc>
```
