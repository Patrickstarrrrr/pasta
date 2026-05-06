//===- FastGuard.cpp -- Lightweight DNF-based path condition --------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#include "Util/FastGuard.h"
#include "Util/PathCond.h"
#include "Util/SVFUtil.h"

using namespace SVF;
using namespace SVFUtil;

FastGuard FastGuard::getTrue()
{
    DNF d;
    d.insert(Clause());
    return FastGuard(d);
}

FastGuard FastGuard::getFalse()
{
    return FastGuard(DNF());
}

FastGuard FastGuard::getAtom(NodeID branchId, bool trueBranch)
{
    DNF d;
    Clause c;
    c.insert(Literal(branchId, trueBranch));
    d.insert(c);
    return FastGuard(d);
}

FastGuard FastGuard::fromPathCond(const PathCond* cond)
{
    if (cond->isTrue())  return getTrue();
    if (cond->isFalse()) return getFalse();
    if (cond->isAtom())  return getAtom(cond->getBranchId(), cond->isTrueBranch());

    FastGuard left = fromPathCond(cond->getLeft());
    FastGuard right = fromPathCond(cond->getRight());
    if (cond->isAnd())
        return AND(left, right);
    else
        return OR(left, right);
}

FastGuard FastGuard::AND(const FastGuard& a, const FastGuard& b)
{
    if (a.isFalse() || b.isFalse()) return getFalse();
    if (a.isTrue())  return b;
    if (b.isTrue())  return a;

    DNF result;
    for (const Clause& ca : a.dnf)
    {
        for (const Clause& cb : b.dnf)
        {
            Clause merged = clauseAnd(ca, cb);
            if (clauseIsSat(merged))
                result.insert(merged);
        }
    }
    simplify(result);
    return FastGuard(result);
}

FastGuard FastGuard::OR(const FastGuard& a, const FastGuard& b)
{
    if (a.isTrue() || b.isTrue()) return getTrue();
    if (a.isFalse()) return b;
    if (b.isFalse()) return a;

    DNF result = a.dnf;
    result.insert(b.dnf.begin(), b.dnf.end());
    simplify(result);
    return FastGuard(result);
}

bool FastGuard::isSat() const
{
    for (const Clause& c : dnf)
    {
        if (clauseIsSat(c))
            return true;
    }
    return false;
}

u32_t FastGuard::depth() const
{
    if (isTrue() || isFalse()) return 0;
    u32_t maxDepth = 0;
    for (const Clause& c : dnf)
    {
        if (c.size() > maxDepth)
            maxDepth = c.size();
    }
    return maxDepth;
}

u32_t FastGuard::atomCount() const
{
    std::set<NodeID> atoms;
    for (const Clause& c : dnf)
    {
        for (const Literal& lit : c)
            atoms.insert(lit.first);
    }
    return atoms.size();
}

std::string FastGuard::toString() const
{
    if (isTrue()) return "T";
    if (isFalse()) return "F";

    std::string s;
    bool firstClause = true;
    for (const Clause& c : dnf)
    {
        if (!firstClause) s += " \\| ";
        firstClause = false;
        if (c.empty())
        {
            s += "T";
            continue;
        }
        s += "(";
        bool firstLit = true;
        for (const Literal& lit : c)
        {
            if (!firstLit) s += " & ";
            firstLit = false;
            s += "c" + std::to_string(lit.first) + (lit.second ? "T" : "F");
        }
        s += ")";
    }
    return s;
}

FastGuard::Clause FastGuard::clauseAnd(const Clause& a, const Clause& b)
{
    Clause result = a;
    result.insert(b.begin(), b.end());
    return result;
}

bool FastGuard::clauseIsSat(const Clause& c)
{
    for (const Literal& lit : c)
    {
        Literal neg(lit.first, !lit.second);
        if (c.find(neg) != c.end())
            return false;
    }
    return true;
}

void FastGuard::removeSubsumed(DNF& dnf)
{
    std::vector<Clause> clauses(dnf.begin(), dnf.end());
    std::vector<bool> removed(clauses.size(), false);

    for (size_t i = 0; i < clauses.size(); ++i)
    {
        if (removed[i]) continue;
        for (size_t j = 0; j < clauses.size(); ++j)
        {
            if (i == j || removed[j]) continue;
            // if clauses[i] is a strict subset of clauses[j], remove j
            if (clauses[j].size() > clauses[i].size())
            {
                bool subset = true;
                for (const Literal& lit : clauses[i])
                {
                    if (clauses[j].find(lit) == clauses[j].end())
                    {
                        subset = false;
                        break;
                    }
                }
                if (subset)
                    removed[j] = true;
            }
        }
    }

    DNF result;
    for (size_t i = 0; i < clauses.size(); ++i)
    {
        if (!removed[i])
            result.insert(clauses[i]);
    }
    dnf.swap(result);
}

void FastGuard::simplify(DNF& dnf)
{
    // Remove contradictory clauses
    DNF filtered;
    for (const Clause& c : dnf)
    {
        if (clauseIsSat(c))
            filtered.insert(c);
    }
    dnf.swap(filtered);

    // Remove subsumed clauses
    removeSubsumed(dnf);

    // If empty clause exists, entire DNF is True
    for (const Clause& c : dnf)
    {
        if (c.empty())
        {
            dnf.clear();
            dnf.insert(Clause());
            return;
        }
    }
}
