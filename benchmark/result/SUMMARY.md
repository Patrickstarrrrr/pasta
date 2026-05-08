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

## Testing Strategy

### Demo Regression: `demo/run_sample.sh`
Randomly sample k cases (default 5) instead of running all 22:
```bash
cd demo && ./run_sample.sh 5
```

### Real-Program Regression: `-sample-aliases=N` (commit 597ac2e)
Instead of printing all O(n²) alias pairs (>10 min on bzip2/zlib), use reservoir
sampling inside wpa:
```bash
wpa -ander -print-aliases -sample-aliases=20000 program.bc
wpa -cond-ander -print-aliases -sample-aliases=20000 program.bc
```
This reduces testing time from >10 minutes to ~10 seconds.

### Sampled Results (n=20000)

| Program | A MayAlias | C MayAlias | Disagreements | Time |
|---------|-----------|-----------|---------------|------|
| `cjson` | 185 (0.9%) | 69 (0.3%) | **116** | ~1.6s |
| `bzip2` | 1750 (8.8%) | 1738 (8.7%) | **12** | ~10s |
| `zlib` | 935 (4.7%) | 929 (4.6%) | **6** | ~17s |

*Disagreements = pairs where Andersen says MayAlias but CondAnder proves NoAlias.*

## Observations

- k=5 provides sufficient precision; increasing k to 10/20/50/100 shows no additional CondPts entries on cjson (stays at 1865).
- The print-aliases phase dominates total time for bzip2/zlib due to O(n²) alias queries with per-query SAT checks.
