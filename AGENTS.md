# Agent Memory: Conditional Andersen Development

> Last updated: 2026-04-29
> This file stores context, fixes, and decisions for future agent sessions.

---

## Current State

- **Project**: PASTA at `/Users/jiayi/PASTA`
- **Build**: CMake, LLVM 17, Z3 4.15.4, `wpa` at `SVF/build/bin/wpa`
- **Key source dirs**: `SVF/svf/include/WPA/`, `SVF/svf/lib/WPA/`, `SVF/svf/include/Util/`

---

## Implemented Features

### Conditional Andersen (`-cond-ander`)
- Base conditional Andersen inheriting from `Andersen`
- Path-condition guards on constraint-graph edges
- Conditional points-to propagation with `condPtsMap`
- Two limit modes: m/n-limit (default) and depth-based k-limit
- FastGuard DNF-based SAT backend + Z3 fallback
- Inter-procedural guards via `connectCaller2CalleeParams`
- Field-sensitive alias via `expandCondFIObjs`

### Conditional Andersen Wave-Diff (`-cond-ander-wave`)
- Inherits `AndersenWaveDiff`
- Same conditional data structures as base
- Differential propagation via `condDiffPtsMap`
- Phase 1: topo-order `processNode` with diff
- Phase 2: `postProcessNode` with load/store + SCC handling

---

## Critical Bug Fixes

### 1. PathCond Pointer Equality → Structural Equality
**File**: `SVF/svf/include/Util/PathCond.h`
**Root cause**: `getOr()` and `containsInOr()` used `==` (pointer equality) for absorption. Since `applyLimits()` creates fresh nodes for structurally identical conditions, absorption failed, OR-trees grew unbounded until depth > 100 triggered `getCappedTrue()` → guards collapsed to ⊤.
**Fix**: Use `*a->getLeft() == *b`, `*tree == *sub` (structural equality).

### 2. Missing CondAndersenWaveDiff_WPA in BVDataPTAImpl
**File**: `SVF/svf/lib/MemoryModel/PointerAnalysisImpl.cpp`
**Root cause**: `ptD` not allocated as `MutDiffPTDataTy` for `CondAndersenWaveDiff_WPA`, causing segfault on `condDiffPtsMap` access.
**Fix**: Add `|| type == CondAndersenWaveDiff_WPA` to constructor condition.

### 3. Missing handleCopyGep in postProcessNode
**File**: `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`
**Root cause**: Preserved SCC sub-nodes' copy/gep edges not propagated in Phase 2, causing unconditional pts divergence.
**Fix**: Add `handleCopyGep(node)` + loop over preserved SCC sub-nodes in `postProcessNode()`.

### 4. Segfault from Removed Sub-Nodes
**File**: `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`
**Root cause**: Sub-nodes removed from constraint graph (e.g., field collapse) after SCC detection.
**Fix**: Add `consCG->hasGNode(subId)` guard before accessing sub-nodes.

### 5. Bitvector Propagation Divergence (k=0 / k=1 with -merge-cond-scc)
**Files**: `SVF/svf/include/WPA/ConditionalAndersenWaveDiff.h`, `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`, `SVF/svf/lib/WPA/ConditionalAndersen.cpp`
**Root cause**: Even with guards truncated to ⊤ or depth≤1, objective metrics (AvgPtsSetSize, TotalObjects, CopyProcessed) diverged from `-ander`.
- `ConditionalAndersen` overrides `getDiffPts()` to return **full pts**, disabling diff propagation.
- `condChanged` in `processCopy/processGep` pushed dst into worklist even when bitvector was unchanged.
- `hasCondPts` in `handleCopyGep` fired `processGep` when diff was empty, calling `getGepObjVar` / `setObjFieldInsensitive` and creating spurious objects.
- `postProcessNode()` extra `handleCopyGep` ran even when `mergeCondSCC=true`, where SCCs were already merged.
**Fix**:
- Re-override `getDiffPts()` in `ConditionalAndersenWaveDiff.h` to restore Andersen diff behavior.
- Skip extra `handleCopyGep` and sub-node handling when `mergeCondSCC=true`.
- `handleCopyGep` only fires on `hasBaseDiff` (diff non-empty).
- Remove `if (condChanged) pushIntoWorklist(dst)` from `processCopy` and `processGep`.

### 6. Cond SCC Merge O(N×M) → O(N)
**Files**: `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`, `SVF/svf/lib/WPA/ConditionalAndersen.cpp`
**Root cause**: `mergeSrcToTgt` scanned the entire `edgeGuards` map (214K entries) on every SCC merge, causing 22-34ms overhead on jq.bc.
**Fix**: Move edgeGuard relocation to a **single bulk pass** in `SCCDetect` after all merges complete. Collect `mergedNodes` during merge loop, then iterate `edgeGuards` once and re-key/re-merge colliding guards.

### 7. True Guard Implicitization (Sparse condPtsMap)
**Files**: `SVF/svf/lib/WPA/ConditionalAndersen.cpp`, `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`, `SVF/svf/include/WPA/ConditionalAndersen.h`, `SVF/svf/include/WPA/ConditionalAndersenWaveDiff.h`
**Root cause**: `processAddr` eagerly added `(obj, True)` for every address-taken object, and `processCopy` propagated these True guards across all copy edges. `condPtsMap` quickly ballooned to contain ~N×M True entries, making iteration in `processCopy`/`processGep`/`processLoad`/`processStore` proportional to the full points-to set size rather than the conditional subset.
**Fix**:
- `processAddr`: no longer writes to `condPtsMap` (True is implicit).
- `orMergeCondPts`: if the merged result is True, erase the entry instead of storing.
- `processCopy`/`processGep`: True edge guards only iterate `condPtsMap[src]` (non-True guards). Non-True edge guards also scan `getPts(src)` for implicit True guards.
- `alias`/`expandCondFIObjs`: iterate the bitvector `pts` intersection, looking up `condPtsMap` with a True fallback.
- `edgeGuards` updates in `processLoad`/`processStore`: skip `getOr` when pointers are identical.
- `processLoad`/`processStore`: replace linear scan over `condPtsMap[pointer]` with O(1) direct lookup.
- `orMergeCondPts`: use `find` before `operator[]` to avoid inserting empty maps.

### 8. Conditional Diff Propagation (WaveDiff)
**File**: `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`
**Root cause**: `condDiffPtsMap` and `currentDiffObjs` existed but were never used. `processCopy`/`processGep` iterated the entire `condPtsMap[node]` on every invocation, even when only 1–2 objects had changed guards.
**Fix**:
- `orMergeCondPts`: inserts the object into `condDiffPtsMap[var]` whenever the guard actually changes.
- `handleCopyGep`: fires when `hasBaseDiff || hasCondDiff` (previously only `hasBaseDiff`).
- `processCopy`/`processGep`: when `node == currentDiffNode && !currentDiffObjs.empty()`, iterate only `currentDiffObjs` (vector, cache-friendly) instead of the full `condPtsMap[node]`. Sub-nodes and non-diff nodes fall back to full scan.
- Implicit True guards on non-True edges still scan `getPts(src)` as before — that path is independent of diffs.

### 9. Timer Measurement (`getClk` vs `getClk(true)`)
**Files**: `SVF/svf/lib/WPA/ConditionalAndersenWaveDiff.cpp`, `SVF/svf/lib/WPA/ConditionalAndersen.cpp`
**Root cause**: `SVFStat::getClk(bool mark)` returns `0.0` when `mark=false` and `Options::MarkedClocksOnly()` is `true` (default). Timer calls without `true` reported zero.
**Fix**: Use `stat->getClk(true)` for all conditional timer measurements.

### 10. Reproducible Alias Sampling
**File**: `SVF/svf/lib/WPA/WPAPass.cpp`
**Root cause**: `rand()` without fixed seed caused different pair sets across runs.
**Fix**: Add `srand(42)` in `PrintAliasPairs` reservoir sampling.

---

## Test Results

### `-cond-ander` vs `-cond-ander-wave` Parity
- **ALL demo/demo2 cases MATCH** (unconditional pts + conditional alias)
- Fixes: PathCond structural equality, missing ptD allocation, missing handleCopyGep, hasGNode checks

### `-nander` vs `-cond-ander` Precision
- **Zero precision loss** across all tests (no NoAlias → MayAlias)
- **102 false positives eliminated** across 14/21 demo cases

### Benchmarks (sample-aliases=50000, fixed seed)

| Program | nander MayAlias | cond-ander MayAlias | Reduced | Time (nander) | Time (cond) |
|---|---|---|---|---|---|
| cjson | 355 | 132 | 223 (62.8%) | 2.33s | 1.55s |
| bzip2 | 4341 | 4340 | 1 (0.02%) | 9.70s | 10.32s |
| tmux | — | — | — | >30min timeout | >30min timeout |

### `-ander` vs `-cond-ander-wave` Metric Parity
After all fixes, **all objective metrics match perfectly** on jq.bc and all demo/demo2 cases:

| Metric | `-ander` | `-cond-ander-wave -k 0 -mcs` | `-cond-ander-wave -k 1 -mcs` |
|---|---|---|---|
| TotalObjects | 17228 | 17228 | 17228 |
| AvgPtsSetSize | 10.8919 | 10.8919 | 10.8919 |
| AvgTopLvlPtsSize | 30.3898 | 30.3898 | 30.3898 |
| CopyProcessed | 169359 | 169359 | 169359 |
| GepProcessed | 49513 | 49513 | 49513 |
| LoadProcessed | 550531 | 550531 | 550531 |
| StoreProcessed | 106263 | 106263 | 106263 |
| SolveIterations | 12 | 12 | 12 |
| NumOfSCCDetect | 12 | 12 | 12 |

### jq.bc Performance (k=3, -merge-cond-scc)

| Metric | Before Optimizations | After Optimizations |
|---|---|---|
| **TotalTime** | 14.578 ms | **9.799 ms** |
| Cond SCC merge | 22.239 ms | **0.366 ms** (60× faster) |
| Cond propagation | 5.740 ms | 5.963 ms |
| Guard limit | 2.053 ms | 2.091 ms |
| Total cond overhead | 30.032 ms | **8.420 ms** |

---

## Default Options

```
kLimit=3, mLimit=0, nLimit=0
useDepthLimit=false
mergeCondSCC=false
eagerSat=false
```

---

## Important Notes for Future Agents

1. **Preserved SCC handling**: When `mergeCondSCC=false`, conditional SCCs are not merged. Sub-nodes remain in CG but may later be removed. Always check `hasGNode(subId)`.

2. **Wave-diff Phase 1 vs Phase 2**: Phase 1 handles copy/gep in topo order. Phase 2 handles load/store + our added `handleCopyGep` for preserved SCC convergence.

3. **PathCond absorption**: Must use structural equality, never pointer equality. `applyLimits` creates fresh nodes.

4. **Alias sampling**: `-sample-aliases` uses `srand(42)` for reproducibility. Without fixed seed, pair sets differ across runs.

5. **Bitvector propagation invariant**: `condPtsMap` and `edgeGuards` are **metadata only**. They must NEVER alter bitvector propagation or worklist scheduling. `condChanged` must never push to worklist. `handleCopyGep` must only fire on actual bitvector diff.

6. **Large program scalability**: tmux (7.2MB bc) exceeds 30min analysis time. Consider smaller sample sizes or `-cond-ander-merge-cond-scc` for speed.

---

## Key Commands

```bash
# Build
make -j$(sysctl -n hw.ncpu) wpa

# Run conditional Andersen
./SVF/build/bin/wpa -cond-ander -cond-ander-k 3 -print-aliases -stat=false -extapi=./SVF/build/lib/extapi.bc <file.bc>

# Run wave-diff variant
./SVF/build/bin/wpa -cond-ander-wave -cond-ander-k 3 -print-aliases -stat=false -extapi=./SVF/build/lib/extapi.bc <file.bc>

# Benchmark with sampling
./SVF/build/bin/wpa -cond-ander -cond-ander-k 3 -print-aliases -sample-aliases 50000 -stat=false -extapi=./SVF/build/lib/extapi.bc benchmark/bc/cjson.bc
```
