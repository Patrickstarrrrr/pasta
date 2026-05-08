# Benchmark: Andersen vs Conditional Andersen

## Phase 1: Pure Analysis Time (no alias printing)

| Program  | Nodes | -ander time | -cond-ander time | slowdown |
|----------|-------|-------------|------------------|----------|
| `cjson`  | 7600  | ~0.5s       | ~0.5s            | ~1.0x    |
| `bzip2`  | 23471 | ~1.5s       | ~2.1s            | ~1.4x    |
| `zlib`   | 30285 | ~2.1s       | ~3.0s            | ~1.4x    |

Notes:
- CondAnder uses default `-cond-ander-use-depth-limit -cond-ander-k=5`.
- Default parameters changed from unlimited (k=0) to k=5 to prevent infinite guard growth on cycles.

## Phase 2: MayAlias Count (with -print-aliases)

| Program  | Andersen MayAlias | CondAnder k=5 MayAlias | Reduction |
|----------|-------------------|------------------------|-----------|
| `cjson`  | 270,880           | 100,038                | 63%       |
| `bzip2`  | >1,461,009*       | >683,478*              | >53%      |
| `zlib`   | (timeout)         | (timeout)              | -         |

\* Partial count from 10-minute run (full count requires >10 min for bzip2/zlib).

## Key Fixes

1. **Default Parameters (commit b8107f1)**:
   - `useDepthLimit`: false → true
   - `kLimit`: 0 (unlimited) → 5
   - Prevents infinite guard growth and timeout on programs with cycles.

2. **PAG Node Removal Crash (commit b8107f1)**:
   - `expandCondFIObjs()` now checks `pag->hasGNode(obj)` before accessing objects.
   - Fixes abort trap when printing aliases after `normalizePointsTo()` removes GepObjVar nodes.

## PhiStmt Guard Design (commit 4622a00)

- **PhiStmt guard**: Changed from per-operand incoming-block guard to phi-BB guard.
- **Rationale**: Experiments on 23 test cases show only 1 case (`05_phi_assign`) with a 1-MayAlias difference; all others are identical. Phi-BB guard is simpler and matches LLVM SSA IR semantics (phi BB guard is usually `True`).
- **Fallback**: Original per-operand implementation is kept in comments in `ConditionalAndersen.cpp` for reference.

## Observations

- k=5 provides sufficient precision; increasing k to 10/20/50/100 shows no additional CondPts entries on cjson (stays at 1865).
- The print-aliases phase dominates total time for bzip2/zlib due to O(n²) alias queries with per-query SAT checks.
