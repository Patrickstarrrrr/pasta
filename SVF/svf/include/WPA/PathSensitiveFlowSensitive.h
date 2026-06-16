//===- PathSensitiveFlowSensitive.h -- Intraprocedural path-sensitive FS ---//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifdef SVF_ENABLE_SPAS

#ifndef PATH_SENSITIVE_FLOWSENSITIVE_H_
#define PATH_SENSITIVE_FLOWSENSITIVE_H_

#include "MemoryModel/CondPointsToDS.h"
#include "WPA/FlowSensitive.h"

namespace SVF
{

/*!
 * @brief Path-/context-/flow-sensitive pointer analysis (SPAS-style).
 *
 * Stores both top-level and address-taken points-to facts under BDD guards,
 * propagates guards along the SVFG, and answers alias queries by checking
 * whether two pointers can point to a common object under a satisfiable guard.
 */
class PathSensitiveFlowSensitive : public FlowSensitive
{
public:
    typedef CondDFPTData CondDFPTDataTy;

    /// Constructor
    explicit PathSensitiveFlowSensitive(SVFIR* _pag)
        : FlowSensitive(_pag, PathS_FSSPARSE_WPA), condDFPTData(nullptr),
          kLimit(Options::PsfsKLimit()), useDepthLimit(Options::PsfsUseDepthLimit()),
          useRefinement(Options::PsfsRefine()), currentK(-1)
    {
    }

    /// Destructor
    ~PathSensitiveFlowSensitive() override
    {
        delete condDFPTData;
    }

    /// Create single instance
    static PathSensitiveFlowSensitive* createPSFSWPA(SVFIR* _pag)
    {
        if (psfspta == nullptr)
        {
            psfspta = std::unique_ptr<PathSensitiveFlowSensitive>(new PathSensitiveFlowSensitive(_pag));
            psfspta->analyze();
        }
        return psfspta.get();
    }

    static void releasePSFSWPA()
    {
        psfspta = nullptr;
    }

    const std::string PTAName() const override
    {
        return "PathSensitiveFlowSensitive";
    }

    static inline bool classof(const PathSensitiveFlowSensitive*)
    {
        return true;
    }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return pta->getAnalysisTy() == PathS_FSSPARSE_WPA;
    }

    /// Initialize analysis
    void initialize() override;

    /// Solve constraints, optionally with a level-by-level refinement loop.
    void solveConstraints() override;

    /// Finalize analysis
    void finalize() override;

protected:
    /// Process each SVFG node (overrides FlowSensitive to use guarded data).
    bool processSVFGNode(SVFGNode* node) override;

    /// Per-statement handlers.
    bool processAddr(const AddrSVFGNode* addr) override;
    bool processCopy(const CopySVFGNode* copy) override;
    bool processGep(const GepSVFGNode* gep) override;
    bool processPhi(const PHISVFGNode* phi) override;
    bool processLoad(const LoadSVFGNode* load) override;
    bool processStore(const StoreSVFGNode* store) override;

    /// Propagate along direct/indirect edges with the edge guard.
    bool propAlongDirectEdge(const DirectSVFGEdge* edge) override;
    bool propAlongIndirectEdge(const IndirectSVFGEdge* edge) override;
    bool propVarPtsFromSrcToDst(NodeID var, const SVFGNode* src, const SVFGNode* dst) override;

    /// Path-sensitive alias query.
    AliasResult alias(NodeID node1, NodeID node2) override;

    /// Guard of an SVFG node (conjunction of branch conditions on the path).
    virtual Guard getNodeGuard(const SVFGNode* node) const;

    /// Guard of an indirect SVFG edge.
    virtual Guard getEdgeGuard(const IndirectSVFGEdge* edge) const;

    /// Apply a guarded store update (weak or strong) to @p obj at @p loc.
    void applyStoreUpdate(CondDFPTDataTy::LocID loc, NodeID obj, const Guard& guard,
                          const PointsTo& pts, bool strongUpdate);

    /// Disjunction of calling-context atoms that may activate @p fun.
    virtual Guard getFunctionContextGuard(const FunObjVar* fun) const;

    /// Absorb a new context atom into the function's active-context guard.
    virtual void absorbContextAtom(const FunObjVar* fun, const Guard& ctx);

    /// Return the edge guard for the pos-th predecessor of a PHI node.
    virtual Guard getPhiOperandGuard(const PHISVFGNode* phi, u32_t pos) const;

    /// Return the unconditional union of conditional facts for @p var at @p loc.
    PointsTo getOverallCondPts(CondDFPTDataTy::LocID loc, NodeID var) const;

    /// Cap a guard to the current k-limit; returns @p g if within budget, else True.
    Guard capGuard(const Guard& g) const;

    /// True if @p obj can be strongly updated at a store (heap/array/field-insensitive/recursive checks).
    bool isStrongUpdatableObject(NodeID obj) const;

    /// Solve once with the current kLimit.
    void solveOnce();

    /// Conditional data-flow points-to data.
    CondDFPTDataTy* condDFPTData;

    /// Guard-size limit (max number of atomic propositions, -1 = unlimited).
    s32_t kLimit;

    /// Whether to enforce kLimit during guard construction.
    bool useDepthLimit;

    /// Whether to run the level-by-level refinement loop.
    bool useRefinement;

    /// Current k during a refinement iteration.
    s32_t currentK;

    /// Active calling-context guard per function (disjunction of context atoms).
    Map<const FunObjVar*, Guard> funContextGuard;

    /// Cached top-level points-to (unconditional) used for top-level vars.
    /// Address-taken vars use condDFPTData.

private:
    static std::unique_ptr<PathSensitiveFlowSensitive> psfspta;
};

} // namespace SVF

#endif // PATH_SENSITIVE_FLOWSENSITIVE_H_

#endif // SVF_ENABLE_SPAS
