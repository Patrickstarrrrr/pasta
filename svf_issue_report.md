## Bug Report: Missing `push()` before `pop()` in `SaberCondAllocator::isSatisfiable`

### Location
**File:** `svf/lib/SABER/SaberCondAllocator.cpp`
**Function:** `bool SaberCondAllocator::isSatisfiable(const Condition &condition)`
**Lines:** 626-635

### Current Code (Buggy)

```cpp
bool SaberCondAllocator::isSatisfiable(const Condition &condition)
{
    Condition::getSolver().add(condition.getExpr());
    z3::check_result result = Condition::getSolver().check();
    Condition::getSolver().pop();   // <-- pop without push!
    if (result == z3::sat || result == z3::unknown)
        return true;
    else
        return false;
}
```

### Problem

The Z3 solver uses a stack-based scope model. `add()` inserts assertions into the **current scope**, and `pop()` removes the top scope. Calling `pop()` without a matching `push()` is undefined behavior — it either:

1. Throws an exception if the solver is at scope level 0, or
2. Accidentally pops a scope pushed by the caller, corrupting the solver state.

### Comparison with Correct Usage

The adjacent function `isEquivalent()` in the same file uses the correct pattern:

```cpp
bool SaberCondAllocator::isEquivalent(const Condition &lhs, const Condition &rhs)
{
    Condition::getSolver().push();   // ✅ push
    Condition::getSolver().add(lhs.getExpr() != rhs.getExpr());
    z3::check_result res = Condition::getSolver().check();
    Condition::getSolver().pop();    // ✅ matching pop
    return res == z3::unsat;
}
```

Similarly, `Z3Expr::simplify()` in `svf/lib/Util/Z3Expr.cpp` also follows the correct `push() -> add() -> check() -> pop()` pattern.

### Proposed Fix

Add `Condition::getSolver().push()` before `add()`:

```cpp
bool SaberCondAllocator::isSatisfiable(const Condition &condition)
{
    Condition::getSolver().push();   // <-- add this line
    Condition::getSolver().add(condition.getExpr());
    z3::check_result result = Condition::getSolver().check();
    Condition::getSolver().pop();
    if (result == z3::sat || result == z3::unknown)
        return true;
    else
        return false;
}
```

### Impact

Any SABER analysis that calls `isSatisfiable()` (e.g., path condition feasibility checks) may silently corrupt the Z3 solver state or crash, leading to incorrect bug detection results.
