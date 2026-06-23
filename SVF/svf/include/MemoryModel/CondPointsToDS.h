//===- CondPointsToDS.h -- Conditional points-to data structure -----------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifdef SVF_ENABLE_SPAS

#ifndef COND_POINTSTO_DS_H_
#define COND_POINTSTO_DS_H_

#include "MemoryModel/PointsTo.h"
#include "Util/Guard.h"
#include "Util/SVFUtil.h"
#include <unordered_map>

namespace SVF
{

/*!
 * @brief Conditional data-flow points-to sets.
 *
 * Maps a location (e.g., SVFG node ID) and variable (e.g., object ID) to a
 * set of (guard, points-to) pairs.  Used by path-sensitive flow-sensitive
 * analyses to keep points-to facts separated by the path/context guard under
 * which they hold.
 */
class CondDFPTData
{
public:
    typedef NodeID LocID;
    typedef NodeID Key;
    typedef PointsTo DataSet;

    /// Map: variable -> guard -> points-to set
    typedef std::unordered_map<Guard, DataSet, Guard::Hash> GuardPtsMap;
    typedef Map<Key, GuardPtsMap> CondPtsMap;
    /// Map: location -> conditional points-to map
    typedef Map<LocID, CondPtsMap> LocCondPtsMap;

private:
    LocCondPtsMap dfInPtsMap;   ///< IN sets at SVFG nodes.
    LocCondPtsMap dfOutPtsMap;  ///< OUT sets at SVFG nodes.

public:
    CondDFPTData() = default;
    ~CondDFPTData() = default;

    /// Clear everything.
    inline void clear()
    {
        dfInPtsMap.clear();
        dfOutPtsMap.clear();
    }

    ///@{
    /// Access the conditional map at a location.
    inline CondPtsMap& getDFInPtsMap(LocID loc)
    {
        return dfInPtsMap[loc];
    }
    inline const CondPtsMap& getDFInPtsMap(LocID loc) const
    {
        auto it = dfInPtsMap.find(loc);
        assert(it != dfInPtsMap.end() && "No IN set at this location");
        return it->second;
    }
    inline CondPtsMap& getDFOutPtsMap(LocID loc)
    {
        return dfOutPtsMap[loc];
    }
    inline const CondPtsMap& getDFOutPtsMap(LocID loc) const
    {
        auto it = dfOutPtsMap.find(loc);
        assert(it != dfOutPtsMap.end() && "No OUT set at this location");
        return it->second;
    }
    ///@}

    ///@{
    /// Access the conditional points-to set for a variable at a location.
    inline GuardPtsMap& getCondInPtsSet(LocID loc, const Key& var)
    {
        return dfInPtsMap[loc][var];
    }
    inline const GuardPtsMap& getCondInPtsSet(LocID loc, const Key& var) const
    {
        auto it = dfInPtsMap.find(loc);
        if (it == dfInPtsMap.end())
        {
            static const GuardPtsMap empty;
            return empty;
        }
        auto jt = it->second.find(var);
        if (jt == it->second.end())
        {
            static const GuardPtsMap empty;
            return empty;
        }
        return jt->second;
    }
    ///@}

    ///@{
    /// Access the conditional points-to set for a variable at a location (OUT).
    inline GuardPtsMap& getCondOutPtsSet(LocID loc, const Key& var)
    {
        return dfOutPtsMap[loc][var];
    }
    inline const GuardPtsMap& getCondOutPtsSet(LocID loc, const Key& var) const
    {
        auto it = dfOutPtsMap.find(loc);
        if (it == dfOutPtsMap.end())
        {
            static const GuardPtsMap empty;
            return empty;
        }
        auto jt = it->second.find(var);
        if (jt == it->second.end())
        {
            static const GuardPtsMap empty;
            return empty;
        }
        return jt->second;
    }
    ///@}

    /// Union @p pts into the conditional set (@p loc, @p var, @p guard).
    /// Returns true if anything changed.
    inline bool unionCondInPts(LocID loc, const Key& var, const Guard& guard, const DataSet& pts)
    {
        if (pts.empty())
            return false;
        auto lit = dfInPtsMap.try_emplace(loc);
        auto vit = lit.first->second.try_emplace(var);
        auto git = vit.first->second.try_emplace(guard, pts);
        if (!git.second)
            return git.first->second |= pts;
        return true;
    }

    inline bool unionCondOutPts(LocID loc, const Key& var, const Guard& guard, const DataSet& pts)
    {
        if (pts.empty())
            return false;
        auto lit = dfOutPtsMap.try_emplace(loc);
        auto vit = lit.first->second.try_emplace(var);
        auto git = vit.first->second.try_emplace(guard, pts);
        if (!git.second)
            return git.first->second |= pts;
        return true;
    }

    /// Compute the unconditional (over-approximate) points-to set for a variable at a location.
    inline DataSet getOverallInPts(LocID loc, const Key& var) const
    {
        DataSet overall;
        for (const auto& pair : getCondInPtsSet(loc, var))
            overall |= pair.second;
        return overall;
    }

    inline DataSet getOverallOutPts(LocID loc, const Key& var) const
    {
        DataSet overall;
        for (const auto& pair : getCondOutPtsSet(loc, var))
            overall |= pair.second;
        return overall;
    }

    /// Propagate conditional IN->IN.
    inline bool updateDFInFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar, const Guard& edgeGuard)
    {
        bool changed = false;
        for (const auto& pair : getCondInPtsSet(srcLoc, srcVar))
        {
            const Guard& g = pair.first;
            const DataSet& pts = pair.second;
            Guard combined = g & edgeGuard;
            if (combined.isSat() && unionCondInPts(dstLoc, dstVar, combined, pts))
                changed = true;
        }
        return changed;
    }

    /// Propagate conditional OUT->IN (used when source is a store).
    inline bool updateDFInFromOut(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar, const Guard& edgeGuard)
    {
        bool changed = false;
        for (const auto& pair : getCondOutPtsSet(srcLoc, srcVar))
        {
            const Guard& g = pair.first;
            const DataSet& pts = pair.second;
            Guard combined = g & edgeGuard;
            if (combined.isSat() && unionCondInPts(dstLoc, dstVar, combined, pts))
                changed = true;
        }
        return changed;
    }

    /// Propagate conditional IN->OUT.
    inline bool updateDFOutFromIn(LocID srcLoc, const Key& srcVar, LocID dstLoc, const Key& dstVar, const Guard& edgeGuard)
    {
        bool changed = false;
        for (const auto& pair : getCondInPtsSet(srcLoc, srcVar))
        {
            const Guard& g = pair.first;
            const DataSet& pts = pair.second;
            Guard combined = g & edgeGuard;
            if (combined.isSat() && unionCondOutPts(dstLoc, dstVar, combined, pts))
                changed = true;
        }
        return changed;
    }

    /// Propagate all conditional OUT facts at @p srcLoc to the IN set at @p dstLoc,
    /// ANDing each guard with @p edgeGuard.  Returns true if anything changed.
    inline bool updateAllDFInFromOut(LocID srcLoc, LocID dstLoc, const Guard& edgeGuard)
    {
        bool changed = false;
        const CondPtsMap& srcMap = getDFOutPtsMap(srcLoc);
        for (const auto& varPair : srcMap)
        {
            const Key& var = varPair.first;
            for (const auto& guardPair : varPair.second)
            {
                Guard combined = guardPair.first & edgeGuard;
                if (combined.isSat() && unionCondInPts(dstLoc, var, combined, guardPair.second))
                    changed = true;
            }
        }
        return changed;
    }

    /// Collect all (guard, points-to) pairs for @p var across every location in
    /// the OUT map.  Used for path-sensitive alias queries.
    inline void collectAllOutPtsForVar(const Key& var, std::vector<std::pair<Guard, DataSet>>& out) const
    {
        for (const auto& locPair : dfOutPtsMap)
        {
            auto it = locPair.second.find(var);
            if (it == locPair.second.end())
                continue;
            for (const auto& guardPair : it->second)
                out.emplace_back(guardPair.first, guardPair.second);
        }
    }

    /// Collect every location in the OUT map that has conditional facts for @p var.
    inline void collectLocationsForVar(const Key& var, Set<LocID>& locs) const
    {
        for (const auto& locPair : dfOutPtsMap)
        {
            if (locPair.second.find(var) != locPair.second.end())
                locs.insert(locPair.first);
        }
    }

    /// Return the number of (loc, var, guard) entries in the OUT map.
    inline u32_t numOutEntries() const
    {
        u32_t n = 0;
        for (const auto& locPair : dfOutPtsMap)
            for (const auto& varPair : locPair.second)
                n += static_cast<u32_t>(varPair.second.size());
        return n;
    }

    /// Return the number of (loc, var, guard) entries in the IN map.
    inline u32_t numInEntries() const
    {
        u32_t n = 0;
        for (const auto& locPair : dfInPtsMap)
            for (const auto& varPair : locPair.second)
                n += static_cast<u32_t>(varPair.second.size());
        return n;
    }

    /// Dump the conditional points-to map (for debugging).
    void dump() const
    {
        SVFUtil::outs() << "=== CondDFPTData IN ===\n";
        for (const auto& locPair : dfInPtsMap)
        {
            SVFUtil::outs() << "Loc " << locPair.first << ":\n";
            for (const auto& varPair : locPair.second)
            {
                SVFUtil::outs() << "  Var " << varPair.first << ":\n";
                for (const auto& condPair : varPair.second)
                {
                    SVFUtil::outs() << "    guard [";
                    condPair.first.dump();
                    SVFUtil::outs() << "] => { ";
                    for (NodeID o : condPair.second)
                        SVFUtil::outs() << o << " ";
                    SVFUtil::outs() << "}\n";
                }
            }
        }
    }
};

} // namespace SVF

#endif // COND_POINTSTO_DS_H_

#endif // SVF_ENABLE_SPAS
