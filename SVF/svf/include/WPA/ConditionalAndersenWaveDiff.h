//===- ConditionalAndersenWaveDiff.h -- Path-aware Andersen pointer analysis---------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef CONDITIONALANDERSENWAVEDIFF_H_
#define CONDITIONALANDERSENWAVEDIFF_H_

#include "WPA/Andersen.h"
#include "Util/PathCond.h"
#include "Util/FastGuard.h"
#include "Util/Z3Expr.h"
#include "Graphs/ICFGEdge.h"
#include <parallel_hashmap/phmap.h>

namespace SVF
{

/*!
 * Conditional Andersen (Phase 1).
 * Extends standard Andersen with path-condition guards on copy edges.
 * Each points-to element is a pair (PathCond, NodeID).
 */
class ConditionalAndersenWaveDiff : public AndersenWaveDiff
{
public:
    /// Conditional points-to: object -> guard (at most one guard per object)
    typedef phmap::flat_hash_map<NodeID, const PathCond*> CondPointsTo;

    /// Constructor
    ConditionalAndersenWaveDiff(SVFIR* _pag,
                        PTATY type = PointerAnalysis::CondAndersenWaveDiff_WPA);

    /// Destructor
    virtual ~ConditionalAndersenWaveDiff() {}

    /// Singleton factory (analogous to AndersenWaveDiff::createAndersenWaveDiff)
    static ConditionalAndersenWaveDiff* createConditionalAndersenWaveDiff(SVFIR* _pag)
    {
        static ConditionalAndersenWaveDiff* instance = nullptr;
        if (instance == nullptr)
        {
            instance = new ConditionalAndersenWaveDiff(_pag);
            instance->analyze();
        }
        return instance;
    }
    static void releaseConditionalAndersenWaveDiff()
    {
        // No-op for singleton lifetime; managed externally
    }


    /// Initialize analysis (disable diff-pts, attach edge guards)
    virtual void initialize() override;

    /// Finalize analysis (print conditional points-to for debugging)
    virtual void finalize() override;

    /// Alias query using conditional points-to sets
    virtual AliasResult alias(NodeID v1, NodeID v2) override;
    virtual AliasResult alias(const SVFVar* v1, const SVFVar* v2) override;

    /// Access conditional points-to for debugging
    const CondPointsTo& getCondPts(NodeID id) const;

    /// classof
    static inline bool classof(const ConditionalAndersenWaveDiff*)
    {
        return true;
    }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return pta->getAnalysisTy() == PointerAnalysis::CondAndersenWaveDiff_WPA;
    }

    virtual const std::string PTAName() const override
    {
        return "ConditionalAndersenWaveDiff";
    }

protected:
    s32_t kLimit;
    bool eagerSat;
    bool useFastGuard;
    bool useDepthLimit;   ///< true = use depth-based k-limit; false = use m/n limits
    u32_t mLimit;         ///< conjunctive length limit (0 = unlimited)
    u32_t nLimit;         ///< disjunctive clause limit (0 = unlimited)
    bool mergeCondSCC;    ///< true = merge SCCs even if they contain conditional edges
    bool aliasNoSat;      ///< true = skip Z3 SAT in alias queries
    /// Guards whose conjunctive chain has reached mLimit.
    /// Any further And-operation with these guards is ignored.
    mutable Set<const PathCond*> conjCappedGuards;

    /// Edge kind for unified edge-guard map
    enum CondEdgeKind { CopyStatic, CopyDerived, Load, Store, Gep };

    struct EdgeGuardKey
    {
        NodeID src;
        NodeID dst;
        CondEdgeKind kind;
        bool operator==(const EdgeGuardKey& o) const
        {
            return src == o.src && dst == o.dst && kind == o.kind;
        }
    };
    struct EdgeGuardKeyHash
    {
        size_t operator()(const EdgeGuardKey& k) const
        {
            return std::hash<NodeID>()(k.src) ^
                   (std::hash<NodeID>()(k.dst) << 1) ^
                   (std::hash<int>()(static_cast<int>(k.kind)) << 2);
        }
    };

    /// Conditional points-to storage: NodeID -> Map<ObjID, PathCond>
    Map<NodeID, CondPointsTo> condPtsMap;

    /// Conditional diff pts: tracks objects whose guards changed since
    /// last time their node was processed. Cleared at node-level in solveWorklist.
    Map<NodeID, Set<NodeID>> condDiffPtsMap;

    /// Current node's diff objects (set by solveWorklist before processNode).
    mutable NodeID currentDiffNode;
    mutable std::vector<NodeID> currentDiffObjs;

    /// Rep nodes of SCCs that were preserved (not merged) during SCCDetect.
    Set<NodeID> preservedSCCReps;

    /// Unified edge-guard map: (src, dst, kind) -> PathCond
    phmap::flat_hash_map<EdgeGuardKey, const PathCond*, EdgeGuardKeyHash> edgeGuards;

    /// Look up an edge guard of a specific kind (returns True if absent)
    const PathCond* getEdgeGuard(NodeID src, NodeID dst, CondEdgeKind kind) const;

    /// Convenience: look up copy edge guard (static > derived > True)
    const PathCond* getEdgeGuard(NodeID src, NodeID dst) const;
    const PathCond* getLoadEdgeGuard(NodeID src, NodeID dst) const;
    const PathCond* getStoreEdgeGuard(NodeID src, NodeID dst) const;
    const PathCond* getGepEdgeGuard(NodeID src, NodeID dst) const;

    /// Override constraint processing
    virtual void processAddr(const AddrCGEdge* addr) override;
    virtual bool processCopy(NodeID node, const ConstraintEdge* edge) override;
    virtual bool processLoad(NodeID node, const ConstraintEdge* load) override;
    virtual bool processStore(NodeID node, const ConstraintEdge* store) override;
    virtual bool processGep(NodeID node, const GepCGEdge* edge) override;
    virtual void handleCopyGep(ConstraintNode* node) override;

    /// Override getDiffPts to restore diff-pts behavior.
    /// The base ConditionalAndersen overrides this to return the full pts set,
    /// which disables diff propagation and causes massive redundant work in
    /// the WaveDiff variant.  We restore the original Andersen behavior here
    /// because our conditional diff is tracked separately (currentDiffObjs).
    virtual inline const PointsTo& getDiffPts(NodeID id) override
    {
        NodeID rep = sccRepNode(id);
        if (Options::DiffPts())
            return getDiffPTDataTy()->getDiffPts(rep);
        else
            return getPTDataTy()->getPts(rep);
    }

    /// Override solveWorklist to add fixpoint iteration for preserved SCCs.
    virtual void solveWorklist() override;

    /// Override processNode to set up conditional diff objects before
    /// AndersenWaveDiff's copy/gep propagation.
    virtual void processNode(NodeID nodeId) override;

    /// Override postProcessNode to set up conditional diff objects before
    /// AndersenWaveDiff's load/store propagation.
    virtual void postProcessNode(NodeID nodeId) override;

    /// Override SCC merge to synchronize conditional points-to and edge guards
    virtual bool mergeSrcToTgt(NodeID srcId, NodeID tgtId) override;

    /// Check if an SCC contains any constraint edge with a conditional guard.
    bool sccHasConditionalEdge(NodeID repId) const;

    /// Override SCC detection to optionally skip conditional-edge SCCs
    virtual NodeStack& SCCDetect() override;

    /// Override inter-procedural edge connection to attach callsite guards

    virtual void connectCaller2CalleeParams(const CallICFGNode* cs,
            const FunObjVar* F, NodePairSet& cpySrcNodes) override;
    virtual void connectCaller2ForkedFunParams(const CallICFGNode* cs,
            const FunObjVar* F, NodePairSet& cpySrcNodes) override;

    /// Extract branch conditions from PhiStmt and attach to copy edges
    void attachStaticEdgeGuards();

    /// Compute path guard for a basic block (OR of incoming conditional edges)
    const PathCond* getBBGuard(const SVFBasicBlock* bb) const;

    /// Expand field-insensitive objects in a conditional points-to set.
    /// True guards are implicit: iterates the bitvector pts of the node.
    CondPointsTo expandCondFIObjs(NodeID nodeId) const;

    /// Apply guard limits. Supports two modes:
    ///   - depth-based (useDepthLimit=true): if AST depth > kLimit, collapse to True.
    ///   - m/n-based (useDepthLimit=false): truncate pure And chains to m literals;
    ///     collapse guards with >n DNF clauses to True.
    const PathCond* applyLimits(const PathCond* cond) const;

    /// Check if a guard has reached the conjunctive length limit (m).
    bool isConjCapped(const PathCond* cond) const
    {
        return conjCappedGuards.count(cond) > 0;
    }

    /// Extract leaves of a pure And chain (in-order traversal).
    std::vector<const PathCond*> extractAndChain(const PathCond* cond) const;

    /// Build a left-associative And chain from a list of literals.
    const PathCond* buildAndChain(const std::vector<const PathCond*>& literals) const;

    /// Count DNF clauses (using FastGuard conversion).
    u32_t countClauses(const PathCond* cond) const;

    /// OR-merge a guard onto an existing (var,obj) entry in condPtsMap.
    /// Returns true iff the entry was newly inserted or the guard changed.
    bool orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard);

    /// Convert PathCond to Z3Expr for SAT checking.
    Z3Expr pathCondToZ3(const PathCond* cond) const;

    /// Z3-based SAT check (with proper push/pop).
    bool z3IsSat(const PathCond* cond) const;

    /// Dump edge guards for debugging
    void dumpEdgeGuards() const;

    /// Sample alias queries on top-level pointers to measure precision gain.
    void sampleAliasQueries(u32_t sampleSize = 100000);

    /// Statistics counters
    mutable u32_t numZ3SatChecks;
    mutable u32_t numAliasRefined;        ///< base MayAlias that stayed MayAlias after cond check
    mutable u32_t numAliasRefinedToNoAlias; ///< base MayAlias that was refined to NoAlias
    mutable u32_t numAliasTotal;
    mutable u32_t numCondPtsEntries;

    // Diff-pts performance counters
    mutable u32_t numDiffPtsHits;      // processCopy/GEP used diff objs
    mutable u32_t numDiffPtsMisses;    // processCopy/GEP fell back to full scan
    mutable u32_t numDiffPtsPropagated; // total objects propagated via diff pts
    mutable u32_t numFullScanPropagated; // total objects propagated via full scan

    /// Simple timing counters for profiling
    mutable double timeCondProp;      // processCopy/Load/Store/Gep conditional logic
    mutable double timeCondAlias;     // alias() conditional query
    mutable double timeCondSCCMerge;  // mergeSrcToTgt conditional pts + edgeGuards
    mutable double timeGuardLimit;    // applyLimits() calls
    mutable double timeSATCheck;      // z3IsSat() calls
};

} // End namespace SVF

#endif // CONDITIONALANDERSENWAVEDIFF_H_
