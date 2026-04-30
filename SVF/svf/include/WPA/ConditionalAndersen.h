//===- ConditionalAndersen.h -- Path-aware Andersen pointer analysis---------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifndef CONDITIONALANDERSEN_H_
#define CONDITIONALANDERSEN_H_

#include "WPA/Andersen.h"
#include "Util/PathCond.h"
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
    typedef OrderedSet<std::pair<const PathCond*, NodeID>> CondPointsTo;

    /// Constructor
    ConditionalAndersen(SVFIR* _pag,
                        PTATY type = PointerAnalysis::CondAndersen_WPA)
        : Andersen(_pag, type), kLimit(1)
    {
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

    /// Conditional points-to storage: NodeID -> Set<(PathCond, ObjID)>
    Map<NodeID, CondPointsTo> condPtsMap;

    /// Guard maps: (src,dst) -> PathCond
    Map<std::pair<NodeID, NodeID>, const PathCond*> staticCopyGuards;
    Map<std::pair<NodeID, NodeID>, const PathCond*> derivedCopyGuards;

    /// Override constraint processing
    virtual void processAddr(const AddrCGEdge* addr) override;
    virtual bool processCopy(NodeID node, const ConstraintEdge* edge) override;
    virtual bool processLoad(NodeID node, const ConstraintEdge* load) override;
    virtual bool processStore(NodeID node, const ConstraintEdge* store) override;

    /// Override to disable differential points-to propagation
    virtual inline const PointsTo& getDiffPts(NodeID id) override
    {
        NodeID rep = sccRepNode(id);
        return getPTDataTy()->getPts(rep);
    }

    /// Override SCC merge to synchronize conditional points-to and edge guards
    virtual bool mergeSrcToTgt(NodeID srcId, NodeID tgtId) override;

    /// Look up the guard for a copy edge
    const PathCond* getEdgeGuard(NodeID src, NodeID dst) const;

    /// Extract branch conditions from PhiStmt and attach to copy edges
    void attachStaticEdgeGuards();

    /// Simple k-limiting: if depth exceeds k, abstract to True
    const PathCond* applyKLimit(const PathCond* cond) const;
};

} // End namespace SVF

#endif // CONDITIONALANDERSEN_H_
