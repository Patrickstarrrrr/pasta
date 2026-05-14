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

### 5. Reproducible Alias Sampling
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

5. **Large program scalability**: tmux (7.2MB bc) exceeds 30min analysis time. Consider smaller sample sizes or `-cond-ander-merge-cond-scc` for speed.

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
