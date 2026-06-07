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
      precisionSampleSize(Options::SingleTrackPrecisionSample()),
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
            if (aliasUseSat && z3IsSat(PathCond::getAnd(p1.second, g2)))
            {
                aliasQueryTime += (SVFStat::getClk(true) - tStart) / TIMEINTERVAL;
                return MayAlias;
            }
            if (!aliasUseSat)
            {
                // Approximate mode: conservatively treat non-True guards as MayAlias
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

    // Clean up condPtsMap: remove objects that are not in the bitvector.
    // These "extra" objects were introduced by the conditional processGep path
    // when the bitvector diff was empty (bitvector path missed the field object).
    // Keeping them breaks the subset invariant: condPtsMap should only track
    // guards for objects already present in the bitvector.
    for (auto it = condPtsMap.begin(); it != condPtsMap.end(); )
    {
        NodeID node = it->first;
        const PointsTo& bvPts = Andersen::getPts(node);
        auto& cmap = it->second;
        for (auto jt = cmap.begin(); jt != cmap.end(); )
        {
            if (!bvPts.test(jt->first))
                jt = cmap.erase(jt);
            else
                ++jt;
        }
        if (cmap.empty())
            it = condPtsMap.erase(it);
        else
            ++it;
    }

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

    // Optional per-pointer precision gain sampling
    if (precisionSampleSize > 0)
        samplePrecisionGain(precisionSampleSize);

    // Optional alias partner reduction sampling
    if (precisionSampleSize > 0)
        sampleAliasPartnerReduction(precisionSampleSize);

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

void SingleTrackCondAndersen::samplePrecisionGain(u32_t sampleSize)
{
    SVFUtil::outs() << "  [samplePrecision] Collecting top-level vars...\n";
    std::vector<NodeID> topVars;
    const auto& varMap = pag->getSVFVarMap();
    for (const auto& p : varMap)
    {
        if (pag->isValidTopLevelPtr(p.second))
            topVars.push_back(p.first);
    }
    if (topVars.empty())
    {
        SVFUtil::outs() << "  [samplePrecision] No top-level vars, skipping.\n";
        return;
    }

    // Random shuffle for sampling
    // Sort to ensure deterministic sampling across different analysis runs.
    std::sort(topVars.begin(), topVars.end());
    std::mt19937 rng(42);
    if (topVars.size() > sampleSize)
    {
        std::shuffle(topVars.begin(), topVars.end(), rng);
        topVars.resize(sampleSize);
    }

    u32_t sampled = static_cast<u32_t>(topVars.size());
    SVFUtil::outs() << "  [samplePrecision] Sampling " << sampled
                    << " top-level pointers...\n";

    u64_t totalBvSize = 0;
    u64_t totalCondSize = 0;
    u64_t refinedCount = 0;
    u64_t totalReduction = 0;
    u64_t maxReduction = 0;
    u64_t extraInCond = 0;      // objects in condPtsMap but not in bitvector
    u64_t missingInCond = 0;    // objects in bitvector but filtered by condPtsMap

    for (NodeID v : topVars)
    {
        NodeID rep = sccRepNode(v);
        size_t bvSize = Andersen::getPts(rep).count();
        size_t condSize = getPts(rep).count();  // triggers lazy sync + False filter

        totalBvSize += bvSize;
        totalCondSize += condSize;

        if (condSize < bvSize)
        {
            refinedCount++;
            u64_t reduction = static_cast<u64_t>(bvSize - condSize);
            totalReduction += reduction;
            if (reduction > maxReduction)
                maxReduction = reduction;
        }

        // Debug: count extra/missing objects
        auto cIt = condPtsMap.find(rep);
        if (cIt != condPtsMap.end())
        {
            const PointsTo& bvPts = Andersen::getPts(rep);
            for (const auto& pair : cIt->second)
            {
                if (pair.second->isFalse()) continue;
                if (!bvPts.test(pair.first))
                    extraInCond++;
            }
        }
        for (NodeID obj : Andersen::getPts(rep))
        {
            if (cIt == condPtsMap.end() || cIt->second.find(obj) == cIt->second.end())
                missingInCond++;
            else if (cIt->second.at(obj)->isFalse())
                missingInCond++;
        }
    }

    // Clear pts cache to free memory (getPts may have cached many sets)
    ptsCache.clear();

    double avgReduction = (refinedCount > 0)
        ? (static_cast<double>(totalReduction) / static_cast<double>(refinedCount))
        : 0.0;

    SVFUtil::outs() << "  [samplePrecision] Done. sampled=" << sampled
                    << " totalBvSize=" << totalBvSize
                    << " totalCondSize=" << totalCondSize
                    << " refined=" << refinedCount
                    << " totalReduction=" << totalReduction
                    << " avgReduction=" << avgReduction
                    << " maxReduction=" << maxReduction
                    << " extraInCond=" << extraInCond
                    << " missingInCond=" << missingInCond << "\n";
}

void SingleTrackCondAndersen::sampleAliasPartnerReduction(u32_t sampleSize)
{
    SVFUtil::outs() << "  [samplePartners] Collecting top-level vars...\n";
    std::vector<NodeID> topVars;
    const auto& varMap = pag->getSVFVarMap();
    for (const auto& p : varMap)
    {
        if (pag->isValidTopLevelPtr(p.second))
            topVars.push_back(p.first);
    }
    if (topVars.empty())
    {
        SVFUtil::outs() << "  [samplePartners] No top-level vars, skipping.\n";
        return;
    }

    std::mt19937 rng(42);
    if (topVars.size() > sampleSize)
    {
        std::shuffle(topVars.begin(), topVars.end(), rng);
        topVars.resize(sampleSize);
    }

    SVFUtil::outs() << "  [samplePartners] Sampling " << topVars.size()
                    << " top-level pointers for alias partner reduction...\n";

    // Pre-sync all top-level pointers so that objects with implicit True guards
    // are present in condPtsMap. This is necessary because condPtsMap only
    // stores non-True guards; absent objects would otherwise be misclassified
    // as NoAlias by alias().
    // NOTE: unsat objects are now stored as False (not erased), so sync will
    // not overwrite them with True.
    SVFUtil::outs() << "  [samplePartners] Syncing top-level pointers to condPtsMap...\n";
    for (NodeID v : topVars)
        ensureNodeSynced(sccRepNode(v));

    // Build reverse points-to map (object -> top-level pointers) using bitvector pts
    SVFUtil::outs() << "  [samplePartners] Building reverse points-to map...\n";
    Map<NodeID, std::vector<NodeID>> revPts;
    for (NodeID v : topVars)
    {
        NodeID rep = sccRepNode(v);
        for (NodeID o : Andersen::getPts(rep))
            revPts[o].push_back(v);
    }

    u64_t totalBvPartners = 0;
    u64_t totalRefined = 0;
    u64_t refinedPtrCount = 0;
    u64_t totalReduction = 0;
    u64_t maxReduction = 0;
    NodeID maxReductionPtr = 0;

    // Distribution buckets
    u64_t bucket0 = 0, bucket1_10 = 0, bucket11_100 = 0, bucket101_1000 = 0, bucket1001plus = 0;

    for (NodeID p : topVars)
    {
        NodeID repP = sccRepNode(p);

        // Collect all bitvector alias partners: pointers sharing at least one object
        std::unordered_set<NodeID> bvPartners;
        for (NodeID o : Andersen::getPts(repP))
        {
            auto it = revPts.find(o);
            if (it != revPts.end())
            {
                for (NodeID q : it->second)
                    if (q != p)
                        bvPartners.insert(q);
            }
        }

        if (bvPartners.empty())
            continue;

        // Check which partners are refined to NoAlias by conditional analysis.
        u64_t refinedForP = 0;
        auto itP = condPtsMap.find(repP);

        for (NodeID q : bvPartners)
        {
            NodeID repQ = sccRepNode(q);
            auto itQ = condPtsMap.find(repQ);

            if (itP == condPtsMap.end() || itQ == condPtsMap.end())
            {
                refinedForP++;
                continue;
            }

            bool mayAlias = false;
            for (const auto& pairP : itP->second)
            {
                if (pairP.second->isFalse()) continue;
                auto jt = itQ->second.find(pairP.first);
                if (jt == itQ->second.end()) continue;
                if (jt->second->isFalse()) continue;

                if (pairP.second->isTrue() && jt->second->isTrue())
                {
                    mayAlias = true;
                    break;
                }
                if (aliasUseSat && z3IsSat(PathCond::getAnd(pairP.second, jt->second)))
                {
                    mayAlias = true;
                    break;
                }
                if (!aliasUseSat)
                {
                    mayAlias = true;
                    break;
                }
            }

            if (!mayAlias)
                refinedForP++;
        }

        u64_t bvCount = bvPartners.size();
        totalBvPartners += bvCount;
        totalRefined += refinedForP;

        if (refinedForP > 0)
        {
            refinedPtrCount++;
            totalReduction += refinedForP;
            if (refinedForP > maxReduction)
            {
                maxReduction = refinedForP;
                maxReductionPtr = repP;
            }
            if (refinedForP <= 10) bucket1_10++;
            else if (refinedForP <= 100) bucket11_100++;
            else if (refinedForP <= 1000) bucket101_1000++;
            else bucket1001plus++;
        }
        else
        {
            bucket0++;
        }


    }

    // Print diagnostic info for the max-reduction pointer
    if (maxReductionPtr != 0)
    {
        auto it = condPtsMap.find(maxReductionPtr);
        u64_t nTrue = 0, nFalse = 0, nConcrete = 0;
        if (it != condPtsMap.end())
        {
            for (const auto& pair : it->second)
            {
                if (pair.second->isTrue()) nTrue++;
                else if (pair.second->isFalse()) nFalse++;
                else nConcrete++;
            }
        }
        SVFUtil::outs() << "  [samplePartners] Max-reduction pointer: " << maxReductionPtr
                        << " reduction=" << maxReduction
                        << " true=" << nTrue << " false=" << nFalse
                        << " concrete=" << nConcrete << "\n";
    }

    SVFUtil::outs() << "  [samplePartners] Refined distribution: 0=" << bucket0
                    << " 1-10=" << bucket1_10
                    << " 11-100=" << bucket11_100
                    << " 101-1000=" << bucket101_1000
                    << " >1000=" << bucket1001plus << "\n";

    SVFUtil::outs() << "  [samplePartners] Done. sampled=" << topVars.size()
                    << " totalBvPartners=" << totalBvPartners
                    << " totalRefined=" << totalRefined;
    if (refinedPtrCount > 0)
    {
        double avgReduction = static_cast<double>(totalReduction)
                              / static_cast<double>(refinedPtrCount);
        SVFUtil::outs() << " refinedPtrs=" << refinedPtrCount
                        << " avgReduction=" << avgReduction
                        << " maxReduction=" << maxReduction;
    }
    SVFUtil::outs() << " (Z3 SAT: " << (aliasUseSat ? "enabled" : "disabled") << ")\n";
}
