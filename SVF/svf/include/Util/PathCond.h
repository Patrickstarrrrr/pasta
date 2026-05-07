//===- PathCond.h -- Path condition representation--------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef PATHCOND_H_
#define PATHCOND_H_

#include "SVFIR/SVFType.h"
#include <set>
#include <vector>
#include <string>

namespace SVF
{

/*!
 * Lightweight path condition for Conditional Andersen.
 * Represents boolean expressions over branch conditions:
 *   - True (unconditional)
 *   - False (unsatisfiable)
 *   - Atom(branchId, trueBranch)  e.g., condA or !condA
 *   - And(a, b)
 *   - Or(a, b)
 *
 * Phase 1: syntactic SAT (detects c /\ !c in conjunctive contexts).
 * Or at the top level is handled by recursive disjunction.
 * And containing Or is handled conservatively (returns true).
 */
class PathCond
{
public:
    enum Kind
    {
        True,
        False,
        Atom,
        And,
        Or
    };

private:
    Kind kind;
    NodeID branchId;   ///< valid when kind == Atom
    bool trueBranch;   ///< valid when kind == Atom
    const PathCond* left;   ///< valid when kind == And or Or
    const PathCond* right;  ///< valid when kind == And or Or

    mutable u32_t cachedDepth;
    mutable bool depthValid;

    PathCond(Kind k, NodeID bid = 0, bool tb = true,
             const PathCond* l = nullptr, const PathCond* r = nullptr)
        : kind(k), branchId(bid), trueBranch(tb), left(l), right(r),
          cachedDepth(0), depthValid(false)
    {
    }

public:
    /// Return the unconditional True path
    static const PathCond* getTrue()
    {
        static PathCond t(True);
        return &t;
    }

    /// Return the unsatisfiable False path
    static const PathCond* getFalse()
    {
        static PathCond f(False);
        return &f;
    }

    /// Return an atomic branch condition
    static const PathCond* getAtom(NodeID branchId, bool trueBranch)
    {
        return new PathCond(Atom, branchId, trueBranch);
    }

    /// Return the conjunction of two path conditions
    static const PathCond* getAnd(const PathCond* a, const PathCond* b)
    {
        if (a->isFalse() || b->isFalse()) return getFalse();
        if (a->isTrue()) return b;
        if (b->isTrue()) return a;
        return new PathCond(And, 0, true, a, b);
    }

    /// Return the disjunction of two path conditions.
    /// Includes simple absorption: if one side is already an OR-tree that
    /// contains the other, return the larger one (avoids AST bloat cycles).
    static const PathCond* getOr(const PathCond* a, const PathCond* b)
    {
        if (a->isTrue() || b->isTrue()) return getTrue();
        if (a->isFalse()) return b;
        if (b->isFalse()) return a;
        if (a == b) return a;
        // Direct absorption: a | (a & c)  == a,  etc. handled by OR containment.
        if (a->isOr() && (a->getLeft() == b || a->getRight() == b)) return a;
        if (b->isOr() && (b->getLeft() == a || b->getRight() == a)) return b;
        // Deeper absorption (bounded depth to keep cost low).
        if (containsInOr(a, b)) return a;
        if (containsInOr(b, a)) return b;
        return new PathCond(Or, 0, true, a, b);
    }

    /// Check whether 'sub' appears anywhere inside 'tree' as an OR-subtree.
    /// Bounded recursion (maxDepth=8) to avoid blow-up on deep trees.
    static bool containsInOr(const PathCond* tree, const PathCond* sub, int maxDepth = 8)
    {
        if (maxDepth <= 0) return false;
        if (tree == sub) return true;
        if (tree->isOr())
            return containsInOr(tree->getLeft(), sub, maxDepth - 1) ||
                   containsInOr(tree->getRight(), sub, maxDepth - 1);
        return false;
    }

    /// Getters
    //@{
    inline Kind getKind() const
    {
        return kind;
    }
    inline bool isTrue() const
    {
        return kind == True;
    }
    inline bool isFalse() const
    {
        return kind == False;
    }
    inline bool isAtom() const
    {
        return kind == Atom;
    }
    inline bool isAnd() const
    {
        return kind == And;
    }
    inline bool isOr() const
    {
        return kind == Or;
    }
    inline NodeID getBranchId() const
    {
        return branchId;
    }
    inline bool isTrueBranch() const
    {
        return trueBranch;
    }
    inline const PathCond* getLeft() const
    {
        return left;
    }
    inline const PathCond* getRight() const
    {
        return right;
    }
    //@}

    /// Syntactic SAT check.
    /// - True / Atom: satisfiable.
    /// - False: unsatisfiable.
    /// - Or(a,b): SAT if either child is SAT.
    /// - And(...): flatten and look for c /\ !c.  If an Or is nested inside,
    ///   we conservatively return true (sound over-approximation).
    bool isSatisfiable() const
    {
        if (isTrue() || isAtom()) return true;
        if (isFalse()) return false;
        if (isOr())
        {
            return left->isSatisfiable() || right->isSatisfiable();
        }

        // isAnd() — flatten conjuncts and look for direct contradictions.
        std::set<std::pair<NodeID, bool>> atoms;
        std::vector<const PathCond*> worklist;
        worklist.push_back(this);

        while (!worklist.empty())
        {
            const PathCond* c = worklist.back();
            worklist.pop_back();

            if (c->isTrue()) continue;
            if (c->isFalse()) return false;
            if (c->isAtom())
            {
                std::pair<NodeID, bool> key(c->getBranchId(), c->isTrueBranch());
                std::pair<NodeID, bool> negKey(c->getBranchId(), !c->isTrueBranch());
                if (atoms.find(negKey) != atoms.end())
                    return false;
                atoms.insert(key);
            }
            else if (c->isAnd())
            {
                worklist.push_back(c->getRight());
                worklist.push_back(c->getLeft());
            }
            else if (c->isOr())
            {
                // Nested Or inside And: would need distributive law or DPLL.
                // Conservatively return true (sound).
                return true;
            }
        }
        return true;
    }

    /// Check if this AST is a pure conjunction chain (all internal nodes are And,
    /// all leaves are Atom/True/False). Used for m-limit truncation.
    bool isPureAndChain() const
    {
        if (isTrue() || isFalse() || isAtom()) return true;
        if (!isAnd()) return false;
        return left->isPureAndChain() && right->isPureAndChain();
    }

    /// Extract all non-trivial leaves of a pure And chain in in-order traversal.
    /// True/False leaves are skipped. Precondition: isPureAndChain() == true.
    void extractAndLeaves(std::vector<const PathCond*>& out) const
    {
        if (isTrue() || isFalse()) return;
        if (isAtom()) { out.push_back(this); return; }
        left->extractAndLeaves(out);
        right->extractAndLeaves(out);
    }

    /// AST height (tree depth) of this path condition.
    /// Atom: 1. And/Or: max(children) + 1. True/False: 0.
    /// This measures nesting depth of the condition.
    /// Result is cached because depth() is called hot (in applyKLimit).
    u32_t depth() const
    {
        if (depthValid) return cachedDepth;
        if (isTrue() || isFalse()) cachedDepth = 0;
        else if (isAtom()) cachedDepth = 1;
        else if (isOr())
        {
            u32_t dl = left->depth();
            u32_t dr = right->depth();
            cachedDepth = (dl > dr ? dl : dr) + 1;
        }
        else // isAnd()
        {
            u32_t dl = left->depth();
            u32_t dr = right->depth();
            cachedDepth = (dl > dr ? dl : dr) + 1;
        }
        depthValid = true;
        return cachedDepth;
    }

    /// Equality (structural)
    bool operator==(const PathCond& other) const
    {
        if (this == &other) return true;
        if (kind != other.kind) return false;
        if (isTrue() || isFalse()) return true;
        if (isAtom())
            return branchId == other.branchId && trueBranch == other.trueBranch;
        return *left == *other.left && *right == *other.right;
    }

    /// Ordering (structural) for OrderedSet
    bool operator<(const PathCond& other) const
    {
        if (this == &other) return false;
        if (kind != other.kind) return kind < other.kind;
        if (isTrue() || isFalse()) return false;
        if (isAtom())
        {
            if (branchId != other.branchId) return branchId < other.branchId;
            return trueBranch < other.trueBranch;
        }
        if (*left < *other.left) return true;
        if (*other.left < *left) return false;
        return *right < *other.right;
    }

    std::string toString() const
    {
        if (isTrue()) return "T";
        if (isFalse()) return "F";
        if (isAtom())
            return "c" + std::to_string(branchId) + (trueBranch ? "T" : "F");
        if (isAnd())
            return "(" + left->toString() + " /\\ " + right->toString() + ")";
        // isOr()
        return "(" + left->toString() + " \\/ " + right->toString() + ")";
    }
};

} // End namespace SVF

#endif // PATHCOND_H_
