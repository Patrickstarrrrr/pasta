# Conditional Andersen: Implementation Summary & Inference Rules

> This document summarizes the complete implementation of Conditional Path-Aware Andersen Pointer Analysis on top of SVF, and presents the formal inference rules in a notation suitable for academic papers.

---

## 1. Overview

We implement a **path-sensitive Andersen-style points-to analysis** inside the SVF framework. The analysis attaches Boolean path-condition guards to constraint-graph edges and propagates conditional points-to sets. Two pointers are reported as *may-alias* only if they share a common object whose joint guard is satisfiable.

### Key Design Decisions

| Aspect               | Decision                                                                       |
| -------------------- | ------------------------------------------------------------------------------ |
| Baseline             | SVF `Andersen` (not `AndersenWaveDiff`)                                        |
| Guard representation | Lightweight AST (`PathCond`) with memoization                                  |
| SAT backend          | FastGuard (DNF-based) as default; Z3 as fallback                               |
| Limit mode (default) | **m/n-limit**: `m` = conjunctive literal limit, `n` = disjunctive clause limit |
| Limit mode (legacy)  | Depth-based k-limit: `depth(And/Or) = max(children) + 1`                       |
| Default limits       | `m=0, n=0` (unlimited, i.e. no truncation)                                     |
| Field sensitivity    | Preserved via `expandCondFIObjs` before alias check                            |

### Two Limit Modes

| Mode                     | Switch                        | Behavior                                                                                                 |
| ------------------------ | ----------------------------- | -------------------------------------------------------------------------------------------------------- |
| **m/n-limit** (default)  | —                             | Pure And chains truncated to most recent `m` unique literals; guards with >`n` DNF clauses collapse to ⊤ |
| **depth-limit** (legacy) | `-cond-ander-use-depth-limit` | AST height > `k` collapses to ⊤                                                                          |

The m/n-limit design is inspired by k-CFA context prefix truncation: it preserves the *most recent* path conditions and is significantly more precise than coarse depth-based truncation.

---

## 2. Core Improvements

### 2.1 FastGuard Backend

- **File**: `SVF/svf/include/Util/FastGuard.h`, `SVF/svf/lib/Util/FastGuard.cpp`
- Converts `PathCond` AST to DNF (`Set<Clause>` where `Clause = Set<Literal>`)
- Checks satisfiability in `O(|clauses| × |literals|)` without Z3 calls
- Falls back to Z3 only when FastGuard reports SAT (conservative)

### 2.2 Unified Edge Guard Map

- Replaced 5 separate maps (`staticCopyGuards`, `derivedCopyGuards`, `loadGuards`, `storeGuards`, `gepGuards`) with a single map keyed by `EdgeGuardKey{src, dst, kind}`
- `CondEdgeKind` enum: `CopyStatic`, `CopyDerived`, `Load`, `Store`, `Gep`

### 2.3 Inter-procedural Guards

- Override `connectCaller2CalleeParams` and `connectCaller2ForkedFunParams`
- Attach callsite BB guard to dynamically added inter-procedural copy edges

### 2.4 Expanded SVFStmt Coverage

`attachStaticEdgeGuards()` now handles:

- `PhiStmt` — per-operand guard from incoming edge
- `CopyStmt` — BB guard
- `LoadStmt` / `StoreStmt` — BB guard
- `GepStmt` — BB guard
- `SelectStmt` — `BB ∧ condAtom` / `BB ∧ ¬condAtom`
- `CallPE` — callsite BB guard
- `RetPE` — function exit BB guard
- `AddrStmt` — BB guard (optional)

### 2.5 Field-Sensitive Alias

- `expandCondFIObjs()` expands field-insensitive base objects to all fields before alias witness search
- Critical for cases like `14_field_branch` (19 refinements)

### 2.6 CLI Options

- `-cond-ander-m` — conjunctive length limit (default: 0 = unlimited)
- `-cond-ander-n` — disjunctive clause limit (default: 0 = unlimited)
- `-cond-ander-use-depth-limit` — use depth-based k-limit instead of m/n
- `-cond-ander-k` — depth limit (only used in depth mode)
- `-cond-ander-eager-sat` — eager Z3 pruning (default: false)
- `-cond-ander-pwc` — PWC detection (default: true)
- `-cond-ander-dump-guards` — dump edge guards
- `-cond-ander-fast-guard` — use FastGuard (default: true)
- `-saber-cond-ander` — use ConditionalAndersen for SABER

### 2.7 Statistics

Printed in `finalize()`:

- Limit mode (m/n or depth)
- m, n, or k values
- Number of conj-capped guards
- Number of Z3 SAT checks
- Number of alias queries refined
- Total alias queries
- Edge guard counts (by kind)
- Conditional points-to map entries

---

## 3. Guard Semantics

### 3.1 Basic Block Guard

```
φ(bb) = ⋁_{e ∈ InEdges(bb), e has condition c}  (c = trueBranch ? c : ¬c)
```

For a basic block with a single conditional predecessor, `φ(bb)` is a single atom. For multiple predecessors (e.g., after loop unrolling), guards are OR-merged.

### 3.2 PathCond AST

```
g ::= ⊤ | ⊥ | atom(bid, true?) | g₁ ∧ g₂ | g₁ ∨ g₂
```

Simplifications (absorption law, structural sharing):

- `g ∧ ⊤ = g`, `g ∨ ⊥ = g`
- `g ∧ g = g`, `g ∨ g = g`
- `g₁ ∨ (g₁ ∧ g₂) = g₁`
- Bounded `containsInOr` check (max depth 8) for deeper absorption

### 3.3 Guard Limiting (Two Modes)

#### Mode A: m/n-Limit (Default)

**m-limit (conjunctive truncation):**

```
applyMLimit(g) =
  { g,                                          if m = 0 (unlimited)
  { truncateAnd(g, m),                           if g is pure And-chain
                                                  and |uniqueLiterals(g)| > m
  { g,                                          otherwise (not pure And-chain)

truncateAnd(g, m):
  1. Extract all leaves of the pure And-chain (in-order traversal)
  2. Deduplicate by (branchId, trueBranch), preserving "most recent" order
     (scan right-to-left, keep first occurrence of each unique literal)
  3. If deduplicated size > m: rebuild And-chain from the most recent m literals
  4. Mark truncated guard as conj-capped (subsequent And operations ignored)
```

**n-limit (disjunctive collapse):**

```
applyNLimit(g) =
  { g,                                          if n = 0 (unlimited)
  { g,                                          if |DNFClauses(g)| ≤ n
  { ⊤,                                          otherwise

where |DNFClauses(g)| = number of clauses in FastGuard DNF conversion
```

**Combined:**

```
applyLimits(g) = applyNLimit(applyMLimit(g))
```

**Conj-capped guards:** Once a guard reaches the conjunctive length limit, it is inserted into `conjCappedGuards`. Any subsequent `And` operation with a conj-capped guard is ignored:

```
if g ∈ conjCappedGuards:
  And(g, g_new) = g     // ignore new conjunct
```

This is a **sound over-approximation**: the capped guard is already a logical super-set of the true guard.

#### Mode B: Depth-Based k-Limit (Legacy)

```
applyDepthLimit(g) =  { g,              if k = 0 (unlimited)
                      { g,              if depth(g) ≤ k
                      { ⊤,              otherwise

where:
  depth(⊤) = depth(⊥) = 0
  depth(atom) = 1
  depth(g₁ ∧ g₂) = max(depth(g₁), depth(g₂)) + 1
  depth(g₁ ∨ g₂) = max(depth(g₁), depth(g₂)) + 1
```

**Why two modes?** The depth-based limit is simple but coarse: a derived guard `edge ∧ cond` has depth 2, so `k=1` truncates *all* derived guards. The m/n-limit preserves more precision by measuring the number of *distinct* conjunctive conditions rather than AST height.

---

## 4. Inference Rules (Paper Notation)

### Notation

| Symbol                      | Meaning                                                          |
| --------------------------- | ---------------------------------------------------------------- |
| `pts(v)`                    | Unconditional points-to set of variable `v`                      |
| `condPts(v)`                | Conditional points-to map: `ObjID → PathCond`                    |
| `condPts(v)[o]`             | Guard under which `v` points to object `o`                       |
| `φ(stmt)` / `φ(bb)`         | Basic-block path guard of the statement                          |
| `edgeGuard(src, dst, kind)` | Guard attached to a constraint-graph edge                        |
| `SAT(g)`                    | Guard `g` is satisfiable (FastGuard or Z3)                       |
| `applyL(g)`                 | `applyLimits(g)` (m/n-mode) or `applyDepthLimit(g)` (depth-mode) |
| `o ↦ g`                     | Object `o` with guard `g`                                        |
| `cap(g)`                    | Guard `g` is conj-capped (reached m-limit)                       |

---

### 4.1 Address (Addr)

```
φ(addrStmt) = g_edge
──────────────────────────────────────── (Addr)
  a ↦ applyL(g_edge)  ∈  condPts(p)
```

> `p = &a` under BB guard `g_edge`. In practice `g_edge` is often ⊤ for top-level allocations.

---

### 4.2 Copy

```
condPts(q)[o] = g_q      edgeGuard(o, p, CopyStatic) = g_edge      cap(g_q) = false
─────────────────────────────────────────────────────────────────────────────────── (Copy-Static)
        condPts(p)[o]  ⊨=  applyL(g_edge ∧ g_q)

condPts(q)[o] = g_q      edgeGuard(o, p, CopyDerived) = g_edge      cap(g_q) = false
──────────────────────────────────────────────────────────────────────────────────── (Copy-Derived)
        condPts(p)[o]  ⊨=  applyL(g_edge ∧ g_q)

condPts(q)[o] = g_q      cap(g_q) = true
─────────────────────────────────────────────────────────────────────── (Copy-Capped)
        condPts(p)[o]  ⊨=  g_q        // ignore new edge guard
```

> `⊨=` denotes OR-merge: `condPts(p)[o] = condPts(p)[o] ∨ applyL(g_edge ∧ g_q)`.  
> If `o` is not yet in `condPts(p)`, it is inserted with guard `applyL(g_edge ∧ g_q)`.  
> The **Copy-Capped** rule preserves soundness by ignoring further conjuncts once the guard has reached its m-limit.

---

### 4.3 Load

```
condPts(q)[o] = g_q          condPts(o)[o'] = g_o          cap(g_q) = false
φ(loadStmt) = g_edge
─────────────────────────────────────────────────────────────────────────────────── (Load)
      condPts(p)[o']  ⊨=  applyL(g_edge ∧ g_q ∧ g_o)
```

> `p = *q`. For each object `o` that `q` may point to, and each object `o'` that `o` (treated as a pointer object) may point to, propagate `o'` to `p` with the conjunction of the load-edge guard, the pointer guard, and the pointee guard.

**Derived edge creation** (intermediate step):

```
condPts(q)[o] = g_q          φ(loadStmt) = g_edge
─────────────────────────────────────────────────────────────────────────────────── (Load-Derived-Edge)
  edgeGuard(o, p, CopyDerived)  ⊨=  g_edge ∧ g_q
```

---

### 4.4 Store

```
condPts(p)[o] = g_p          condPts(q)[o'] = g_q          cap(g_p) = false
φ(storeStmt) = g_edge
─────────────────────────────────────────────────────────────────────────────────── (Store)
      condPts(o)[o']  ⊨=  applyL(g_edge ∧ g_p ∧ g_q)
```

> `*p = q`. For each object `o` that `p` may point to, add `q`'s points-to objects to `o`'s points-to set.

**Derived edge creation** (intermediate step):

```
condPts(p)[o] = g_p          φ(storeStmt) = g_edge
─────────────────────────────────────────────────────────────────────────────────── (Store-Derived-Edge)
  edgeGuard(q, o, CopyDerived)  ⊨=  g_edge ∧ g_p
```

---

### 4.5 Field-Sensitive Access (Gep)

```
condPts(q)[o] = g_q          φ(gepStmt) = g_edge
fieldMap(o, f) = o_f
─────────────────────────────────────────────────────────────────────────────────── (Gep)
      condPts(p)[o_f]  ⊨=  applyL(g_edge ∧ g_q)
```

> `p = &q->f`. `fieldMap` translates a base object to its field object. If `o` is field-insensitive, `expandCondFIObjs` first expands it to all fields.

---

### 4.6 Phi Node

```
condPts(a)[o] = g_a          φ(pred_a → phiBB) = g_a_edge
─────────────────────────────────────────────────────────────────────────────────── (Phi-Op)
      condPts(p)[o]  ⊨=  applyL(g_a_edge ∧ g_a)
```

> `p = phi(a, b, ...)`. Each operand `a` is guarded by the path condition of its incoming edge.

---

### 4.7 Function Call (CallPE)

```
condPts(arg)[o] = g_arg          φ(callsiteBB) = g_cs
─────────────────────────────────────────────────────────────────────────────────── (CallPE)
      condPts(param)[o]  ⊨=  applyL(g_cs ∧ g_arg)
```

> Inter-procedural parameter binding. The callsite BB guard is attached to the dynamically created copy edge.

---

### 4.8 Function Return (RetPE)

```
condPts(retval)[o] = g_ret          φ(exitBB) = g_exit
─────────────────────────────────────────────────────────────────────────────────── (RetPE)
      condPts(caller_ret)[o]  ⊨=  applyL(g_exit ∧ g_ret)
```

> Return value propagation. The function exit BB guard captures the path condition under which the return statement is reached.

---

### 4.9 Select

```
condPts(a)[o] = g_a          φ(selectBB) = g_bb
─────────────────────────────────────────────────────────────────────────────────── (Select-True)
      condPts(p)[o]  ⊨=  applyL(g_bb ∧ c ∧ g_a)

condPts(b)[o] = g_b          φ(selectBB) = g_bb
─────────────────────────────────────────────────────────────────────────────────── (Select-False)
      condPts(p)[o]  ⊨=  applyL(g_bb ∧ ¬c ∧ g_b)
```

> `p = c ? a : b`. Each branch is guarded by the conjunction of the BB guard and the select condition (or its negation).

---

### 4.10 Alias Check

```
∃ o :  condPts(v₁)[o] = g₁   ∧   condPts(v₂)[o] = g₂   ∧   SAT(g₁ ∧ g₂)
─────────────────────────────────────────────────────────────────────────── (Alias-May)
                        alias(v₁, v₂) = MayAlias

∀ o ∈ (condPts(v₁) ∩ condPts(v₂)) :  UNSAT(condPts(v₁)[o] ∧ condPts(v₂)[o])
─────────────────────────────────────────────────────────────────────────── (Alias-No)
                        alias(v₁, v₂) = NoAlias
```

> Before alias check, `expandCondFIObjs` expands field-insensitive base objects to all their fields. The conjunction `g₁ ∧ g₂` is checked via FastGuard (default) or Z3.

---

### 4.11 SCC Collapse (Cycle Merge)

```
SCC = {v₁, v₂, ..., vₙ}        condPts(vᵢ)[o] = gᵢ   for each vᵢ ∈ SCC
─────────────────────────────────────────────────────────────────────────── (SCC-Merge)
      condPts(rep)[o]  ⊨=  ⋁_{vᵢ ∈ SCC} gᵢ
```

> When Andersen detects a cycle (positive-weight cycle), all nodes in the SCC are merged into a representative `rep`. The conditional points-to sets are OR-merged. This is a **sound over-approximation**.

---

## 5. Experimental Results

### 5.1 Demo Cases (19 programs)

| Case                  | Vanilla | CondAnder (unlimited) | Refined |
| --------------------- | ------- | --------------------- | ------- |
| `01_basic_branch`     | 20      | 19                    | 1       |
| `04_nested_branch`    | 31      | 30                    | 1       |
| `05_phi_assign`       | 24      | 23                    | 1       |
| `07_deep_nesting`     | 44      | 43                    | 1       |
| `08_interproc_direct` | 50      | 46                    | 4       |
| `10_interproc_return` | 105     | 101                   | 4       |
| `14_field_branch`     | 92      | 73                    | **19**  |
| `21_guard_xor`        | 17      | 13                    | 4       |
| `23_multistage_prop`  | 81      | 68                    | **13**  |
| ...                   |         |                       |         |

**Total refined pairs: 55**

### 5.2 Limit Mode Comparison

On `21_guard_xor` (2 distinct conjunctive conditions):

| Mode          | Param   | MayAlias | Refined vs Vanilla |
| ------------- | ------- | -------- | ------------------ |
| Vanilla       | —       | 17       | 0                  |
| **Unlimited** | —       | **13**   | **4**              |
| Depth         | k=1     | 16       | 1                  |
| **MN**        | **m=2** | **13**   | **4**              |

On `23_multistage_prop` (5 distinct conjunctive conditions):

| Mode          | Param   | MayAlias | Refined           |
| ------------- | ------- | -------- | ----------------- |
| Vanilla       | —       | 81       | 0                 |
| **Unlimited** | —       | **68**   | **13**            |
| Depth         | k=1     | 81       | 0                 |
| Depth         | k=2     | 79       | 2                 |
| Depth         | k=3     | 70       | 11                |
| Depth         | k=4     | 79       | 2 ← non-monotonic |
| Depth         | k=5     | 68       | **13**            |
| MN            | m=1     | 81       | 0                 |
| MN            | m=2     | 81       | 0                 |
| MN            | m=3     | 81       | 0                 |
| MN            | m=4     | 78       | 3                 |
| **MN**        | **m=5** | **68**   | **13**            |
| MN            | m≥5     | 68       | **13**            |

**Key insight**: MN-m=2 matches unlimited precision on all small demos, while Depth-k=1 loses precision on 6 cases (`10`, `11`, `14`, `21`, `22`, `23`). This is because derived guards (`edge ∧ cond`) have depth 2 but only 1–2 distinct conjuncts.

### 5.3 Full Demo Sweep (MN-m=2 vs Depth-k=1)

| Case                  | Vanilla | Unlimited | **MN-m=2** | Depth-k=1 |
| --------------------- | ------- | --------- | ---------- | --------- |
| `01_basic_branch`     | 20      | 19        | **19**     | 19        |
| `04_nested_branch`    | 31      | 30        | **30**     | 30        |
| `05_phi_assign`       | 24      | 23        | **23**     | 23        |
| `07_deep_nesting`     | 44      | 43        | **43**     | 43        |
| `08_interproc_direct` | 50      | 46        | **46**     | 46        |
| `10_interproc_return` | 105     | 101       | **101**    | 104       |
| `11_layered_store`    | 65      | 64        | **64**     | 65        |
| `14_field_branch`     | 92      | 73        | **73**     | 88        |
| `21_guard_xor`        | 17      | 13        | **13**     | 16        |
| `22_deep_xor`         | 29      | 25        | **25**     | 28        |
| `23_multistage_prop`  | 81      | 68        | 81         | 81        |

MN-m=2 is **strictly more precise** than Depth-k=1 on 6 cases and equivalent on all others.

---

## 6. Known Issues

### 6.1 Non-Monotonic Depth-Limit Behavior

Increasing `k` does not always monotonically increase precision under depth-mode. For example, in `23_multistage_prop`:

- k=3 → 70 MayAlias
- k=4 → 79 MayAlias (worse!)
- k=5 → 68 MayAlias (better again)

**Root cause**: Andersen’s `solveWorklist` uses `NodeSet` / `NodeBS` containers whose iteration order depends on memory addresses. Different `k` values create different numbers of `PathCond` objects, changing the heap layout, which in turn changes the worklist processing order and SCC collapse timing.

**Mitigation**: Use m/n-limit mode (default). It is less sensitive to AST shape changes because truncation is based on distinct literal count, not tree height.

### 6.2 getBBGuard Only Returns Single-Block Conditions

`getBBGuard(bb)` computes the OR of all conditional incoming **ICFG edges** to `bb`. It does **not** recursively accumulate nested branch conditions. Therefore, a statement inside a 10-level nested `if` still receives a guard of depth 1 (the innermost branch condition). Deeper guards only arise from `And(loadG, ptsG)` in `processLoad/Store` or from multi-stage pointer propagation.

### 6.3 OR-Merge Absorption

Because conditional points-to sets use OR-merge (`condPts(v)[o] = old ∨ new`), unconditional assignments (`guard = ⊤`) absorb all conditional assignments to the same object. This means Conditional Andersen can only add precision for *newly introduced* objects, not refine existing unconditional points-to facts.

---

## 7. File Changes

### New Files

- `SVF/svf/include/Util/FastGuard.h` — DNF-based SAT backend
- `SVF/svf/lib/Util/FastGuard.cpp`
- `demo/code/20_ultra_deep_nesting.c` — 15-level nesting stress test
- `demo/code/21_guard_xor.c` — m-limit stress test (guard XOR)
- `demo/code/22_deep_xor.c` — 5-level nested XOR
- `demo/code/23_multistage_prop.c` — multi-stage propagation stress test

### Modified Files

- `SVF/svf/include/Util/PathCond.h` — `isPureAndChain()`, `extractAndLeaves()`, depth() uses true tree height
- `SVF/svf/include/WPA/ConditionalAndersen.h` — mLimit, nLimit, useDepthLimit, conjCappedGuards, applyLimits
- `SVF/svf/lib/WPA/ConditionalAndersen.cpp` — core solver logic with mn-limit and depth-limit dual mode
- `SVF/svf/include/Util/Options.h` — new CLI flags (`-cond-ander-m`, `-cond-ander-n`, `-cond-ander-use-depth-limit`)
- `SVF/svf/lib/Util/Options.cpp` — option definitions
- `SVF/svf/lib/SABER/SrcSnkDDA.cpp` — `-saber-cond-ander` integration

---

## 8. Quick Reference

### Build

```bash
cd SVF/build && cmake .. && cmake --build . -j$(nproc)
```

### Run Demo

```bash
cd demo && bash run_all.sh
```

### Run with m/n-limit (default mode)

```bash
./SVF/build/bin/wpa -cond-ander -cond-ander-m=3 -cond-ander-n=5 -print-aliases file.bc
```

### Run with depth-limit (legacy mode)

```bash
./SVF/build/bin/wpa -cond-ander -cond-ander-use-depth-limit -cond-ander-k=2 -print-aliases file.bc
```

### Dump Guards

```bash
./SVF/build/bin/wpa -cond-ander -cond-ander-dump-guards file.bc
```

### SABER with Conditional Andersen

```bash
./SVF/build/bin/saber -saber-cond-ander file.bc
```
