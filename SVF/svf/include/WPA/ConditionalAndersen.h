//===- ConditionalAndersen.h -- Path-aware Andersen pointer analysis---------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef CONDITIONALANDERSEN_H_
#define CONDITIONALANDERSEN_H_

#include "WPA/Andersen.h"
#include "Util/PathCond.h"
#include "Util/FastGuard.h"
#include "Util/Z3Expr.h"
#include "Graphs/ICFGEdge.h"

namespace SVF
{

/*!
 * Conditional Andersen (Phase 1).
 * Extends standard Andersen with path-condition guards on copy edges.
 * Each points-to element is a pair (PathCond, NodeID).
 */
class ConditionalAndersen : public Andersen
{
public:
    /// Conditional points-to: object -> guard (at most one guard per object)
    typedef Map<NodeID, const PathCond*> CondPointsTo;

    /// Constructor
    ConditionalAndersen(SVFIR* _pag,
                        PTATY type = PointerAnalysis::CondAndersen_WPA);

    /// Destructor
    virtual ~ConditionalAndersen() {}

    /// Singleton factory (analogous to AndersenWaveDiff::createAndersenWaveDiff)
    static ConditionalAndersen* createConditionalAndersen(SVFIR* _pag)
    {
        static ConditionalAndersen* instance = nullptr;
        if (instance == nullptr)
        {
            instance = new ConditionalAndersen(_pag);
            instance->analyze();
        }
        return instance;
    }
    static void releaseConditionalAndersen()
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
    static inline bool classof(const ConditionalAndersen*)
    {
        return true;
    }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return pta->getAnalysisTy() == PointerAnalysis::CondAndersen_WPA;
    }

    virtual const std::string PTAName() const override
    {
        return "ConditionalAndersen";
    }

protected:
    u32_t kLimit;
    bool eagerSat;
    bool useFastGuard;

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

    /// Unified edge-guard map: (src, dst, kind) -> PathCond
    std::unordered_map<EdgeGuardKey, const PathCond*, EdgeGuardKeyHash> edgeGuards;

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

    /// Override to disable differential points-to propagation
    virtual inline const PointsTo& getDiffPts(NodeID id) override
    {
        NodeID rep = sccRepNode(id);
        return getPTDataTy()->getPts(rep);
    }

    /// Override SCC merge to synchronize conditional points-to and edge guards
    virtual bool mergeSrcToTgt(NodeID srcId, NodeID tgtId) override;

    /// Override inter-procedural edge connection to attach callsite guards
    virtual void connectCaller2CalleeParams(const CallICFGNode* cs,
            const FunObjVar* F, NodePairSet& cpySrcNodes) override;
    virtual void connectCaller2ForkedFunParams(const CallICFGNode* cs,
            const FunObjVar* F, NodePairSet& cpySrcNodes) override;

    /// Extract branch conditions from PhiStmt and attach to copy edges
    void attachStaticEdgeGuards();

    /// Compute path guard for a basic block (OR of incoming conditional edges)
    const PathCond* getBBGuard(const SVFBasicBlock* bb) const;

    /// Expand field-insensitive objects in a conditional points-to set
    CondPointsTo expandCondFIObjs(const CondPointsTo& pts) const;

    /// Simple k-limiting: if depth exceeds k, abstract to True
    const PathCond* applyKLimit(const PathCond* cond) const;

    /// OR-merge a guard onto an existing (var,obj) entry in condPtsMap.
    /// Returns true iff the entry was newly inserted or the guard changed.
    bool orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard);

    /// Convert PathCond to Z3Expr for SAT checking.
    Z3Expr pathCondToZ3(const PathCond* cond) const;

    /// Z3-based SAT check (with proper push/pop).
    bool z3IsSat(const PathCond* cond) const;

    /// Dump edge guards for debugging
    void dumpEdgeGuards() const;

    /// Statistics counters
    mutable u32_t numZ3SatChecks;
    mutable u32_t numAliasRefined;
    mutable u32_t numAliasTotal;
    mutable u32_t numCondPtsEntries;
};

} // End namespace SVF

#endif // CONDITIONALANDERSEN_H_
