//===----------------------------------------------------------------------===//
//
// SingleTrackCondAndersen.cpp -- Single-track conditional Andersen
//
//===----------------------------------------------------------------------===//

#include "WPA/SingleTrackCondAndersen.h"
#include "Util/SVFUtil.h"
#include "SVFIR/SVFIR.h"

using namespace SVF;

SingleTrackCondAndersen::SingleTrackCondAndersen(SVFIR* _pag, PTATY type)
    : ConditionalAndersenWaveDiff(_pag, type), analysisComplete(false)
{
}

/// Ensure condPtsMap[node] contains every object in bitvector pts[node].
/// Missing objects are added with True guard.
void SingleTrackCondAndersen::ensureNodeSynced(NodeID node)
{
    const PointsTo& pts = Andersen::getPts(node);
    if (pts.empty())
        return;

    auto it = condPtsMap.find(node);
    if (it == condPtsMap.end())
    {
        CondPointsTo newMap;
        newMap.reserve(pts.count());
        for (NodeID obj : pts)
            newMap[obj] = PathCond::getTrue();
        condPtsMap[node] = std::move(newMap);
        invalidatePtsCache(node);
    }
    else
    {
        bool changed = false;
        for (NodeID obj : pts)
        {
            if (it->second.find(obj) == it->second.end())
            {
                it->second[obj] = PathCond::getTrue();
                changed = true;
            }
        }
        if (changed)
            invalidatePtsCache(node);
    }
}

/// During analysis: return bitvector pts (fast, complete).
/// After analysis: return condPtsMap-derived pts (lazily synced to match bitvector set size).
const PointsTo& SingleTrackCondAndersen::getPts(NodeID id)
{
    id = sccRepNode(id);
    if (!analysisComplete)
        return Andersen::getPts(id);

    auto it = ptsCache.find(id);
    if (it != ptsCache.end())
        return it->second;

    // Lazy sync: ensure condPtsMap contains all objs from bitvector pts
    ensureNodeSynced(id);

    PointsTo pts;
    auto cIt = condPtsMap.find(id);
    if (cIt != condPtsMap.end())
    {
        for (const auto& pair : cIt->second)
        {
            if (!pair.second->isFalse())
                pts.set(pair.first);
        }
    }
    return ptsCache[id] = std::move(pts);
}

const PointsTo& SingleTrackCondAndersen::getPts(NodeID id) const
{
    id = sccRepNode(id);
    if (!analysisComplete)
        return Andersen::getPts(id);

    auto it = ptsCache.find(id);
    if (it != ptsCache.end())
        return it->second;

    // const version: build from condPtsMap (may miss objs not yet synced)
    PointsTo pts;
    auto cIt = condPtsMap.find(id);
    if (cIt != condPtsMap.end())
    {
        for (const auto& pair : cIt->second)
        {
            if (!pair.second->isFalse())
                pts.set(pair.first);
        }
    }
    auto res = ptsCache.emplace(id, std::move(pts));
    return res.first->second;
}

/// Alias query using condPtsMap with Z3 refinement after analysis.
/// Both nodes are lazily synced before querying.
AliasResult SingleTrackCondAndersen::alias(NodeID v1, NodeID v2)
{
    if (v1 == v2)
        return MustAlias;
    NodeID n1 = sccRepNode(v1);
    NodeID n2 = sccRepNode(v2);
    if (n1 == n2)
        return MustAlias;

    if (!analysisComplete)
        return ConditionalAndersenWaveDiff::alias(n1, n2);

    ensureNodeSynced(n1);
    ensureNodeSynced(n2);

    auto it1 = condPtsMap.find(n1);
    auto it2 = condPtsMap.find(n2);
    if (it1 == condPtsMap.end() || it2 == condPtsMap.end())
        return NoAlias;

    for (const auto& p1 : it1->second)
    {
        if (p1.second->isFalse())
            continue;
        auto jt = it2->second.find(p1.first);
        if (jt != it2->second.end())
        {
            const PathCond* g2 = jt->second;
            if (g2->isFalse())
                continue;
            if (p1.second->isTrue() && g2->isTrue())
                return MayAlias;
            if (z3IsSat(PathCond::getAnd(p1.second, g2)))
                return MayAlias;
        }
    }
    return NoAlias;
}

AliasResult SingleTrackCondAndersen::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1 == v2)
        return MustAlias;
    return alias(v1->getId(), v2->getId());
}

void SingleTrackCondAndersen::finalize()
{
    // Print bitvector stats (same as Andersen::finalize).
    Andersen::finalize();

    // Switch to condPtsMap mode for post-analysis queries.
    analysisComplete = true;
    ptsCache.clear();

    // Count conditional points-to entries
    numCondPtsEntries = 0;
    for (const auto& entry : condPtsMap)
        numCondPtsEntries += entry.second.size();

    SVFUtil::outs() << "\n========== SingleTrackCondAndersen Statistics ==========\n";
    SVFUtil::outs() << "  analysisComplete:    true\n";
    SVFUtil::outs() << "  CondPts entries:     " << numCondPtsEntries << "\n";
    SVFUtil::outs() << "========================================================\n\n";
    SVFUtil::outs().flush();
}
