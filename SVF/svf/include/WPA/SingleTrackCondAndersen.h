//===----------------------------------------------------------------------===//
//
// SingleTrackCondAndersen.h -- Single-track conditional Andersen
//
// Based on ConditionalAndersenWaveDiff. During analysis we reuse the base
// class's fast bitvector propagation and diff-based cond propagation (True
// guards are erased as usual). After analysis completes, condPtsMap is
// back-filled with any objects present in the bitvector pts but missing from
// condPtsMap (assigned True guard). From that point on getPts() and alias()
// derive their answers solely from condPtsMap, making it the single source of
// truth while retaining baseline metric parity.
//
//===----------------------------------------------------------------------===//

#ifndef SINGLETRACK_COND_ANDERSEN_H_
#define SINGLETRACK_COND_ANDERSEN_H_

#include "WPA/ConditionalAndersenWaveDiff.h"
#include "Util/SVFStat.h"
#include <unordered_map>

namespace SVF
{

class SingleTrackCondAndersen : public ConditionalAndersenWaveDiff
{
public:
    SingleTrackCondAndersen(SVFIR* _pag, PTATY type = PointerAnalysis::SingleTrackCondAndersen_WPA);

    virtual void finalize() override;

    /// Return pts from condPtsMap after analysis; bitvector pts during analysis
    virtual const PointsTo& getPts(NodeID id) override;
    virtual const PointsTo& getPts(NodeID id) const override;

    /// Alias using condPtsMap with Z3 refinement after analysis
    virtual AliasResult alias(NodeID v1, NodeID v2) override;
    virtual AliasResult alias(const SVFVar* v1, const SVFVar* v2) override;

    static inline bool classof(const SingleTrackCondAndersen*) { return true; }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return pta->getAnalysisTy() == PointerAnalysis::SingleTrackCondAndersen_WPA;
    }
    virtual const std::string PTAName() const override { return "SingleTrackCondAndersen"; }

protected:
    mutable std::unordered_map<NodeID, PointsTo> ptsCache;
    bool analysisComplete;
    u32_t aliasSampleSize;           ///< number of alias pairs to sample (0 = disable)
    bool aliasUseSat;                ///< use Z3 SAT in alias sampling
    u32_t precisionSampleSize;       ///< number of top-level ptrs to sample for precision (0 = disable)

    /// Statistics: alias query timing (post-analysis only)
    u64_t aliasQueryCount;           ///< number of alias() calls after analysis
    double aliasQueryTime;           ///< total time spent in alias() (seconds)

    /// Statistics: condPtsMap size details
    u64_t condPtsMapNodes;           ///< number of nodes in condPtsMap
    u64_t condPtsMapMaxSize;         ///< max entries per node
    u64_t condPtsMapMinSize;         ///< min entries per node (non-empty)

    /// Ensure condPtsMap[node] contains every object in bitvector pts[node]
    void ensureNodeSynced(NodeID node);

    void invalidatePtsCache(NodeID id) { ptsCache.erase(id); }

    /// Sample alias queries on top-level pointers and report precision gain
    void sampleAliasQueries(u32_t sampleSize);

    /// Sample top-level pointers and report per-pointer precision gain
    void samplePrecisionGain(u32_t sampleSize);

    /// Sample top-level pointers and report alias partner reduction
    void sampleAliasPartnerReduction(u32_t sampleSize);
};

} // namespace SVF

#endif // SINGLETRACK_COND_ANDERSEN_H_
