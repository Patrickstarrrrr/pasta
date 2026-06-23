//===- Guard.h -- Path/context guard abstraction for SPAS ------------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifdef SVF_ENABLE_SPAS

#ifndef GUARD_H_
#define GUARD_H_

#include "SVFIR/SVFType.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace SVF
{

/// Hash support for std::pair (used by Guard atom table).
struct PairHash
{
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const
    {
        return std::hash<T1>{}(p.first) ^ (std::hash<T2>{}(p.second) << 1);
    }
};

} // namespace SVF

#include "cuddObj.hh"

namespace SVF
{

/*!
 * @brief Boolean guard over branch conditions and calling contexts.
 *
 * A guard is a BDD (if CUDD is available) or a PathCond-style AST fallback.
 * Each atomic proposition is either:
 *   - a path atom:  a branch condition variable (SVFVar NodeID) being true/false
 *   - a context atom: a call-site (CallICFGNode ID) being active in the call stack
 *
 * Guards are immutable and reference-counted by the underlying BDD manager.
 */
class Guard
{
public:
    enum AtomKind
    {
        PathAtom,     ///< Branch condition from an ICFG edge.
        ContextAtom,  ///< Call-site activation for context sensitivity.
    };

private:
#ifdef SVF_ENABLE_SPAS
    class Manager
    {
    public:
        static Manager& get();

        /// Return the BDD for atom (kind, id, trueBranch), creating a new BDD variable on first use.
        BDD atom(AtomKind kind, NodeID id, bool trueBranch);

        /// Return the constant true/false BDDs.
        const BDD& trueBdd() const { return trueBdd_; }
        const BDD& falseBdd() const { return falseBdd_; }

        /// Return the BDD cube of all context-atom variables.
        const BDD& contextCube() const { return contextCube_; }

        int numVars() const { return cudd.ReadSize(); }

    private:
        Manager();
        ~Manager();
        Manager(const Manager&) = delete;
        Manager& operator=(const Manager&) = delete;

        Cudd cudd;
        BDD trueBdd_;
        BDD falseBdd_;
        BDD contextCube_; ///< Cube of all context-atom BDD variables for existential quantification.
        // Cache for atom BDDs so repeated requests return the same DdNode.
        std::unordered_map<int, BDD> atomCache;
        std::mutex mutex;
        std::unordered_map<std::pair<int, NodeID>, int, PairHash> varIndexMap;
    };

    BDD bdd;
    bool hasCtxAtom;
#else
    // Fallback path: wrap the existing PathCond infrastructure.
    const class PathCond* cond;
#endif

#ifdef SVF_ENABLE_SPAS
    explicit Guard(const BDD& b, bool ctx = false) : bdd(b), hasCtxAtom(ctx) {}
#else
    explicit Guard(const PathCond* c) : cond(c) {}
#endif

public:
    /// Default constructor creates the unconditional true guard.
    Guard() : Guard(getTrue()) {}

    /// Return the unconditional true guard.
    static const Guard& getTrue();

    /// Return the unconditional false guard.
    static const Guard& getFalse();

    /// Return pointer to the singleton true guard (for APIs that use raw pointers).
    static const Guard* getTruePtr() { return &getTrue(); }

    /// Return pointer to the singleton false guard.
    static const Guard* getFalsePtr() { return &getFalse(); }

    /// Intern a guard value and return a stable pointer.
    static const Guard* intern(const Guard& g);

    /// Return an atomic guard for a path/context proposition.
    static Guard atom(AtomKind kind, NodeID id, bool trueBranch = true);

    /// Logical operations.
    Guard operator&(const Guard& other) const;
    Guard operator|(const Guard& other) const;
    Guard operator!() const;

    /// In-place logical operations.
    Guard& operator&=(const Guard& other);
    Guard& operator|=(const Guard& other);

    /// Queries.
    bool isTrue() const;
    bool isFalse() const;
    bool isSat() const;

    /// Implication: this -> other.
    bool implies(const Guard& other) const;

    /// Existentially quantify out all context atoms.
    Guard withoutContextAtoms() const;

    /// Return true if this guard does not depend on any context atom (cheap flag check).
    bool isContextIndependent() const;

    bool operator==(const Guard& other) const;
    bool operator!=(const Guard& other) const;

    /// Hash support for use in unordered containers.
    struct Hash
    {
        std::size_t operator()(const Guard& g) const;
    };

    /// Print the guard to stderr (for debugging).
    void dump() const;

    /// Number of BDD nodes (structural size).
    int nodeCount() const;

    /// Number of atomic propositions this guard depends on.
    int supportSize() const;

#ifdef SVF_ENABLE_SPAS
    /// Access the underlying BDD (CUDD builds only).
    const BDD& getBdd() const { return bdd; }
#else
    /// Access the underlying PathCond (fallback builds only).
    const PathCond* getCond() const { return cond; }
#endif
};

} // namespace SVF

#endif // GUARD_H_

#endif // SVF_ENABLE_SPAS
