//===- SingleTrackCondAndersen.h -- Single-track conditional Andersen --------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef SINGLETRACKCONDANDERSEN_H_
#define SINGLETRACKCONDANDERSEN_H_

#include "WPA/Andersen.h"
#include "Util/PathCond.h"
#include "Graphs/ICFGEdge.h"
#include <parallel_hashmap/phmap.h>

namespace SVF
{

/*!
 * Single-track conditional Andersen.
 *
 * Unlike ConditionalAndersen (which calls base Andersen::processXxx first,
 * then does a second pass over condPtsMap), this class uses condPtsMap as
 * the *sole* propagation data structure.  Bitvector pts are only generated
 * lazily (via getPts) for external compatibility.
 */
class SingleTrackCondAndersen : public Andersen
{
public:
    /// Conditional points-to: object -> guard
    typedef phmap::flat_hash_map<NodeID, const PathCond*> CondPointsTo;

    SingleTrackCondAndersen(SVFIR* _pag,
        PTATY type = PointerAnalysis::SingleTrackCondAndersen_WPA);

    virtual ~SingleTrackCondAndersen() {}

    static SingleTrackCondAndersen* createSingleTrackCondAndersen(SVFIR* _pag)
    {
        static SingleTrackCondAndersen* instance = nullptr;
        if (instance == nullptr)
        {
            instance = new SingleTrackCondAndersen(_pag);
            instance->analyze();
        }
        return instance;
    }

    virtual void initialize() override;
    virtual void finalize() override;

    /// Alias query using conditional points-to sets directly.
    virtual AliasResult alias(NodeID v1, NodeID v2) override;
    virtual AliasResult alias(const SVFVar* v1, const SVFVar* v2) override;

    /// Lazy bitvector generation from condPtsMap.
    virtual const PointsTo& getPts(NodeID id) override;
    virtual const PointsTo& getPts(NodeID id) const override;

    /// Access conditional points-to for debugging
    const CondPointsTo& getCondPts(NodeID id) const;

    static inline bool classof(const SingleTrackCondAndersen*)
    {
        return true;
    }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return pta->getAnalysisTy() == PointerAnalysis::SingleTrackCondAndersen_WPA;
    }

    virtual const std::string PTAName() const override
    {
        return "SingleTrackCondAndersen";
    }

protected:
    s32_t kLimit;
    bool eagerSat;
    bool mergeCondSCC;

    /// Conditional points-to storage: NodeID -> Map<ObjID, PathCond>
    Map<NodeID, CondPointsTo> condPtsMap;

    /// Lazy bitvector cache for getPts().  Invalidated on condPtsMap changes.
    mutable std::unordered_map<NodeID, PointsTo> ptsCache;

    /// Override solving to use condPtsMap-only propagation.
    virtual void solveWorklist() override;
    virtual void processNode(NodeID nodeId) override;

    /// Single-track constraint processing (no base Andersen calls).
    virtual void processAddr(const AddrCGEdge* addr) override;
    virtual bool processCopy(NodeID node, const ConstraintEdge* edge) override;
    virtual bool processLoad(NodeID node, const ConstraintEdge* load) override;
    virtual bool processStore(NodeID node, const ConstraintEdge* store) override;
    virtual bool processGep(NodeID node, const GepCGEdge* edge) override;

    /// Handle load/store edges in Phase 2 (private helper).
    void handleLoadStorePhase2(NodeID nodeId);

    /// SCC merge: merge condPtsMap and invalidate cache.
    virtual bool mergeSrcToTgt(NodeID srcId, NodeID tgtId) override;
    virtual NodeStack& SCCDetect() override;

    /// Attach path-condition guards to static edges.
    void attachStaticEdgeGuards();
    const PathCond* getBBGuard(const SVFBasicBlock* bb) const;

    /// OR-merge a guard onto condPtsMap[var][obj].  Returns true if changed.
    bool orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard);

    /// Invalidate cached bitvector for a node.
    void invalidatePtsCache(NodeID id) { ptsCache.erase(id); }

    /// Z3 SAT check (shared implementation with ConditionalAndersen).
    bool z3IsSat(const PathCond* cond) const;

    /// Statistics
    mutable u32_t numZ3SatChecks;
    mutable u32_t numCondPtsEntries;
};

} // End namespace SVF

#endif // SINGLETRACKCONDANDERSEN_H_
