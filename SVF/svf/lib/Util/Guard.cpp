//===- Guard.cpp -- Path/context guard abstraction for SPAS ----------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifdef SVF_ENABLE_SPAS

#include "Util/Guard.h"

#ifdef SVF_ENABLE_SPAS
#include "Util/Options.h"
#include <sstream>
#else
#include "Util/PathCond.h"
#endif

using namespace SVF;

#ifdef SVF_ENABLE_SPAS

Guard::Manager::Manager()
    : cudd(0, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0, nullptr),
      trueBdd_(cudd.bddOne()),
      falseBdd_(cudd.bddZero()),
      contextCube_(cudd.bddOne())
{
}

Guard::Manager::~Manager() = default;

Guard::Manager& Guard::Manager::get()
{
    static Manager mgr;
    return mgr;
}

BDD Guard::Manager::atom(AtomKind kind, NodeID id, bool trueBranch)
{
    Manager& mgr = get();
    std::lock_guard<std::mutex> lock(mgr.mutex);

    auto key = std::make_pair(static_cast<int>(kind), id);
    auto it = mgr.varIndexMap.find(key);
    int idx;
    if (it != mgr.varIndexMap.end())
    {
        idx = it->second;
    }
    else
    {
        idx = static_cast<int>(mgr.varIndexMap.size());
        mgr.varIndexMap.emplace(key, idx);
        // Optionally assign a human-readable name for debugging.
        std::ostringstream oss;
        oss << (kind == PathAtom ? "p" : "c") << "_" << id;
        mgr.cudd.pushVariableName(oss.str());
    }

    // Ensure the BDD manager has at least idx+1 variables.
    while (mgr.cudd.ReadSize() <= idx)
    {
        mgr.cudd.bddVar();
    }

    int cacheKey = (idx << 1) | (trueBranch ? 1 : 0);
    auto cit = mgr.atomCache.find(cacheKey);
    if (cit != mgr.atomCache.end())
        return cit->second;

    BDD var = mgr.cudd.bddVar(idx);
    BDD result = trueBranch ? var : !var;
    mgr.atomCache.emplace(cacheKey, result);

    if (kind == ContextAtom)
        mgr.contextCube_ = mgr.contextCube_ & var;

    return result;
}

const Guard& Guard::getTrue()
{
    static Guard g(Manager::get().trueBdd());
    return g;
}

const Guard& Guard::getFalse()
{
    static Guard g(Manager::get().falseBdd());
    return g;
}

const Guard* Guard::intern(const Guard& g)
{
    static std::unordered_set<Guard, Guard::Hash> pool;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = pool.find(g);
    if (it != pool.end())
        return &(*it);
    auto ins = pool.insert(g);
    return &(*ins.first);
}

Guard Guard::atom(AtomKind kind, NodeID id, bool trueBranch)
{
    return Guard(Manager::get().atom(kind, id, trueBranch), kind == ContextAtom);
}

Guard Guard::operator&(const Guard& other) const
{
    return Guard(bdd * other.bdd, hasCtxAtom || other.hasCtxAtom);
}

Guard Guard::operator|(const Guard& other) const
{
    return Guard(bdd + other.bdd, hasCtxAtom || other.hasCtxAtom);
}

Guard Guard::operator!() const
{
    return Guard(!bdd, hasCtxAtom);
}

Guard& Guard::operator&=(const Guard& other)
{
    bdd = bdd * other.bdd;
    hasCtxAtom = hasCtxAtom || other.hasCtxAtom;
    return *this;
}

Guard& Guard::operator|=(const Guard& other)
{
    bdd = bdd + other.bdd;
    hasCtxAtom = hasCtxAtom || other.hasCtxAtom;
    return *this;
}

bool Guard::isTrue() const
{
    return bdd.IsOne();
}

bool Guard::isFalse() const
{
    return bdd.IsZero();
}

bool Guard::isSat() const
{
    return !bdd.IsZero();
}

bool Guard::implies(const Guard& other) const
{
    // a -> b  <=>  !a | b
    BDD imp = (!bdd) + other.bdd;
    return imp.IsOne();
}

Guard Guard::withoutContextAtoms() const
{
    return Guard(bdd.ExistAbstract(Manager::get().contextCube()), false);
}

bool Guard::isContextIndependent() const
{
    return !hasCtxAtom;
}

bool Guard::operator==(const Guard& other) const
{
    return bdd == other.bdd;
}

bool Guard::operator!=(const Guard& other) const
{
    return !(bdd == other.bdd);
}

std::size_t Guard::Hash::operator()(const Guard& g) const
{
    return std::hash<DdNode*>{}(g.bdd.getNode());
}

void Guard::dump() const
{
    bdd.print(Manager::get().numVars(), 2);
}

int Guard::nodeCount() const
{
    return bdd.nodeCount();
}

int Guard::supportSize() const
{
    return bdd.SupportSize();
}

#else // !SVF_ENABLE_SPAS

const Guard& Guard::getTrue()
{
    static Guard g(PathCond::getTrue());
    return g;
}

const Guard& Guard::getFalse()
{
    static Guard g(PathCond::getFalse());
    return g;
}

Guard Guard::atom(AtomKind kind, NodeID id, bool trueBranch)
{
    // PathCond only supports path atoms; context atoms are folded into path atoms.
    (void)kind;
    return Guard(PathCond::getAtom(id, trueBranch));
}

Guard Guard::operator&(const Guard& other) const
{
    return Guard(PathCond::getAnd(cond, other.cond));
}

Guard Guard::operator|(const Guard& other) const
{
    return Guard(PathCond::getOr(cond, other.cond));
}

Guard Guard::operator!() const
{
    // PathCond does not have a public negate; approximate using implication?
    // For the fallback we represent negation as the atom with inverted branch.
    // This is conservative for non-atomic guards.
    if (cond->isTrue())
        return getFalse();
    if (cond->isFalse())
        return getTrue();
    return Guard(PathCond::getOr(cond, PathCond::getFalse()));
}

Guard& Guard::operator&=(const Guard& other)
{
    cond = PathCond::getAnd(cond, other.cond);
    return *this;
}

Guard& Guard::operator|=(const Guard& other)
{
    cond = PathCond::getOr(cond, other.cond);
    return *this;
}

bool Guard::isTrue() const
{
    return cond->isTrue();
}

bool Guard::isFalse() const
{
    return cond->isFalse();
}

bool Guard::isSat() const
{
    return cond->isSatisfiable();
}

bool Guard::implies(const Guard& other) const
{
    // a -> b  <=>  !a | b.  Conservative fallback.
    return PathCond::getOr(cond, other.cond)->isTrue();
}

bool Guard::operator==(const Guard& other) const
{
    return cond == other.cond;
}

bool Guard::operator!=(const Guard& other) const
{
    return cond != other.cond;
}

std::size_t Guard::Hash::operator()(const Guard& g) const
{
    return std::hash<const PathCond*>{}(g.cond);
}

void Guard::dump() const
{
    cond->dump();
}

#endif // SVF_ENABLE_SPAS

#endif // SVF_ENABLE_SPAS
