//===- FastGuard.h -- Lightweight DNF-based path condition ------------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef FASTGUARD_H_
#define FASTGUARD_H_

#include "SVFIR/SVFType.h"
#include <set>
#include <vector>
#include <string>

namespace SVF
{

class PathCond;

/*!
 * FastGuard: a lightweight DNF-based path condition backend.
 *
 * Represents boolean formulas as DNF (disjunction of conjunctions):
 *   OR( AND(atom1, atom2, ...), AND(atom3, ...), ... )
 *
 * SAT checking is purely syntactic (detects c ∧ !c in a clause).
 * No Z3 calls — suitable for hot-path propagation and alias queries.
 */
class FastGuard
{
public:
    using Literal = std::pair<NodeID, bool>;   // (branchId, trueBranch)
    using Clause = std::set<Literal>;          // conjunction (ordered for hashing)
    using DNF = std::set<Clause>;              // disjunction

private:
    DNF dnf;

    FastGuard(const DNF& _dnf) : dnf(_dnf) {}

public:
    /// Constructors
    FastGuard() : dnf() {}
    FastGuard(const FastGuard& other) = default;
    FastGuard& operator=(const FastGuard& other) = default;

    /// Factory methods
    static FastGuard getTrue();
    static FastGuard getFalse();
    static FastGuard getAtom(NodeID branchId, bool trueBranch);
    static FastGuard fromPathCond(const PathCond* cond);

    /// Boolean operations
    static FastGuard AND(const FastGuard& a, const FastGuard& b);
    static FastGuard OR(const FastGuard& a, const FastGuard& b);

    /// Queries
    inline bool isTrue() const
    {
        return dnf.size() == 1 && dnf.begin()->empty();
    }
    inline bool isFalse() const
    {
        return dnf.empty();
    }
    bool isSat() const;

    /// Approximate "depth" for k-limiting: max clause size
    u32_t depth() const;

    /// Number of unique atoms across all clauses
    u32_t atomCount() const;

    std::string toString() const;

    bool operator==(const FastGuard& other) const
    {
        return dnf == other.dnf;
    }
    bool operator<(const FastGuard& other) const
    {
        return dnf < other.dnf;
    }

    const DNF& getDNF() const
    {
        return dnf;
    }

private:
    /// Compute conjunction of two clauses (union of literals)
    static Clause clauseAnd(const Clause& a, const Clause& b);

    /// Check if a clause is contradictory (contains c and !c)
    static bool clauseIsSat(const Clause& c);

    /// Remove subsumed clauses (X ⊂ Y  =>  Y is subsumed by X)
    static void removeSubsumed(DNF& dnf);

    /// Simplify DNF: remove contradictions and subsumed clauses
    static void simplify(DNF& dnf);
};

} // End namespace SVF

#endif // FASTGUARD_H_
