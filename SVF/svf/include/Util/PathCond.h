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
 *   - Atom(branchId, trueBranch)  e.g., condA or !condA
 *   - And(a, b)
 *
 * Phase 1: simple syntactic SAT (detects c /\ !c).
 */
class PathCond
{
public:
    enum Kind
    {
        True,
        Atom,
        And
    };

private:
    Kind kind;
    NodeID branchId;   ///< valid when kind == Atom
    bool trueBranch;   ///< valid when kind == Atom
    const PathCond* left;   ///< valid when kind == And
    const PathCond* right;  ///< valid when kind == And

    PathCond(Kind k, NodeID bid = 0, bool tb = true,
             const PathCond* l = nullptr, const PathCond* r = nullptr)
        : kind(k), branchId(bid), trueBranch(tb), left(l), right(r)
    {
    }

public:
    /// Return the unconditional True path
    static const PathCond* getTrue()
    {
        static PathCond t(True);
        return &t;
    }

    /// Return an atomic branch condition
    static const PathCond* getAtom(NodeID branchId, bool trueBranch)
    {
        return new PathCond(Atom, branchId, trueBranch);
    }

    /// Return the conjunction of two path conditions
    static const PathCond* getAnd(const PathCond* a, const PathCond* b)
    {
        if (a->isTrue()) return b;
        if (b->isTrue()) return a;
        return new PathCond(And, 0, true, a, b);
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
    inline bool isAtom() const
    {
        return kind == Atom;
    }
    inline bool isAnd() const
    {
        return kind == And;
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

    /// Syntactic SAT check: detects direct contradictions like c /\ !c
    bool isSatisfiable() const
    {
        if (isTrue()) return true;
        if (isAtom()) return true;

        std::set<std::pair<NodeID, bool>> atoms;
        std::vector<const PathCond*> worklist;
        worklist.push_back(this);

        while (!worklist.empty())
        {
            const PathCond* c = worklist.back();
            worklist.pop_back();

            if (c->isTrue()) continue;
            if (c->isAtom())
            {
                std::pair<NodeID, bool> key(c->getBranchId(), c->isTrueBranch());
                std::pair<NodeID, bool> negKey(c->getBranchId(), !c->isTrueBranch());
                if (atoms.find(negKey) != atoms.end())
                    return false;
                atoms.insert(key);
            }
            else
            {
                worklist.push_back(c->getRight());
                worklist.push_back(c->getLeft());
            }
        }
        return true;
    }

    /// Number of atomic conditions in this path condition
    u32_t depth() const
    {
        if (isTrue()) return 0;
        if (isAtom()) return 1;
        return left->depth() + right->depth();
    }

    /// Equality (structural)
    bool operator==(const PathCond& other) const
    {
        if (this == &other) return true;
        if (kind != other.kind) return false;
        if (isTrue()) return true;
        if (isAtom())
            return branchId == other.branchId && trueBranch == other.trueBranch;
        return *left == *other.left && *right == *other.right;
    }

    /// Ordering (structural) for OrderedSet
    bool operator<(const PathCond& other) const
    {
        if (this == &other) return false;
        if (kind != other.kind) return kind < other.kind;
        if (isTrue()) return false;
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
        if (isAtom())
            return "c" + std::to_string(branchId) + (trueBranch ? "T" : "F");
        return "(" + left->toString() + " /\\ " + right->toString() + ")";
    }
};

} // End namespace SVF

#endif // PATHCOND_H_
