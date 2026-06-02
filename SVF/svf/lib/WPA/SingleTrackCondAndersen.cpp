//===----------------------------------------------------------------------===//
//
// SingleTrackCondAndersen.cpp -- Single-track conditional Andersen
//
//===----------------------------------------------------------------------===//

#include "WPA/SingleTrackCondAndersen.h"
#include "Util/SVFUtil.h"
#include "Util/Options.h"
#include "SVFIR/SVFIR.h"
#include <random>

using namespace SVF;

SingleTrackCondAndersen::SingleTrackCondAndersen(SVFIR* _pag, PTATY type)
    : ConditionalAndersenWaveDiff(_pag, type), analysisComplete(false),
      aliasSampleSize(Options::SingleTrackAliasSample()),
      aliasUseSat(Options::SingleTrackAliasSat()),
      aliasQueryCount(0), aliasQueryTime(0.0),
      condPtsMapNodes(0), condPtsMapMaxSize(0), condPtsMapMinSize(0)
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

    double tStart = SVFStat::getClk(true);
    aliasQueryCount++;

    ensureNodeSynced(n1);
    ensureNodeSynced(n2);

    auto it1 = condPtsMap.find(n1);
    auto it2 = condPtsMap.find(n2);
    if (it1 == condPtsMap.end() || it2 == condPtsMap.end())
    {
        aliasQueryTime += (SVFStat::getClk(true) - tStart) / TIMEINTERVAL;
        return NoAlias;
    }

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
            {
                aliasQueryTime += (SVFStat::getClk(true) - tStart) / TIMEINTERVAL;
                return MayAlias;
            }
            if (z3IsSat(PathCond::getAnd(p1.second, g2)))
            {
                aliasQueryTime += (SVFStat::getClk(true) - tStart) / TIMEINTERVAL;
                return MayAlias;
            }
        }
    }
    aliasQueryTime += (SVFStat::getClk(true) - tStart) / TIMEINTERVAL;
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

    // Count conditional points-to entries and compute detailed stats
    numCondPtsEntries = 0;
    condPtsMapNodes = 0;
    condPtsMapMaxSize = 0;
    condPtsMapMinSize = 0;
    bool first = true;
    for (const auto& entry : condPtsMap)
    {
        u64_t sz = entry.second.size();
        if (sz == 0) continue;
        numCondPtsEntries += sz;
        condPtsMapNodes++;
        if (sz > condPtsMapMaxSize) condPtsMapMaxSize = sz;
        if (first || sz < condPtsMapMinSize)
        {
            condPtsMapMinSize = sz;
            first = false;
        }
    }

    // Optional alias pair sampling
    if (aliasSampleSize > 0)
        sampleAliasQueries(aliasSampleSize);

    SVFUtil::outs() << "\n========== SingleTrackCondAndersen Statistics ==========\n";
    SVFUtil::outs() << "  analysisComplete:    true\n";
    SVFUtil::outs() << "  CondPtsMap nodes:    " << condPtsMapNodes << "\n";
    SVFUtil::outs() << "  CondPtsMap entries:  " << numCondPtsEntries << "\n";
    if (condPtsMapNodes > 0)
    {
        double avg = static_cast<double>(numCondPtsEntries) / static_cast<double>(condPtsMapNodes);
        SVFUtil::outs() << "  CondPtsMap avg:      " << avg << "\n";
        SVFUtil::outs() << "  CondPtsMap max:      " << condPtsMapMaxSize << "\n";
        SVFUtil::outs() << "  CondPtsMap min:      " << condPtsMapMinSize << "\n";
    }
    if (aliasQueryCount > 0)
    {
        double avgMs = (aliasQueryTime * 1000.0) / static_cast<double>(aliasQueryCount);
        SVFUtil::outs() << "  Alias queries:       " << aliasQueryCount << "\n";
        SVFUtil::outs() << "  Alias total time:    " << aliasQueryTime << "s\n";
        SVFUtil::outs() << "  Alias avg time:      " << avgMs << "ms\n";
    }
    SVFUtil::outs() << "========================================================\n\n";
    SVFUtil::outs().flush();
}

void SingleTrackCondAndersen::sampleAliasQueries(u32_t sampleSize)
{
    SVFUtil::outs() << "  [sampleAlias] Collecting top-level vars...\n";
    std::vector<NodeID> topVars;
    const auto& varMap = pag->getSVFVarMap();
    for (const auto& p : varMap)
    {
        if (pag->isValidTopLevelPtr(p.second))
            topVars.push_back(p.first);
    }
    if (topVars.size() < 2)
    {
        SVFUtil::outs() << "  [sampleAlias] Too few top-level vars (" << topVars.size() << "), skipping.\n";
        return;
    }
    SVFUtil::outs() << "  [sampleAlias] Top-level vars: " << topVars.size() << ", generating pairs...\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, topVars.size() - 1);

    u32_t baseMay = 0;
    u32_t refinedToNoAlias = 0;
    u32_t stayedMayAlias = 0;

    for (u32_t i = 0; i < sampleSize; ++i)
    {
        size_t a = dist(rng);
        size_t b = dist(rng);
        if (a == b) { if (++b >= topVars.size()) b = 0; }

        NodeID v1 = topVars[a];
        NodeID v2 = topVars[b];
        NodeID n1 = sccRepNode(v1);
        NodeID n2 = sccRepNode(v2);

        if (!Andersen::getPts(n1).intersects(Andersen::getPts(n2)))
            continue;

        baseMay++;

        // Use the class alias() method so that alias query timing is captured.
        AliasResult r = alias(v1, v2);
        if (r == MayAlias)
            stayedMayAlias++;
        else
            refinedToNoAlias++;
    }

    SVFUtil::outs() << "  [sampleAlias] Done. baseMayAlias=" << baseMay
                    << " refinedToNoAlias=" << refinedToNoAlias
                    << " stayedMayAlias=" << stayedMayAlias
                    << " (Z3 SAT: " << (aliasUseSat ? "enabled" : "disabled") << ")\n";
}
