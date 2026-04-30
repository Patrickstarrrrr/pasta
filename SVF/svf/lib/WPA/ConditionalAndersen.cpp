//===- ConditionalAndersen.cpp -- Path-aware Andersen analysis--------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#include "WPA/ConditionalAndersen.h"
#include "SVFIR/SVFStatements.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"

using namespace SVF;
using namespace SVFUtil;

/*!
 * Initialize analysis.
 * Disable differential points-to to ensure processCopy is always invoked.
 */
void ConditionalAndersen::initialize()
{
    Andersen::initialize();
    attachStaticEdgeGuards();
}

/*!
 * Attach path-condition guards to static copy edges derived from PhiStmt.
 * Phase 1: only handles PhiStmt (sufficient for the slides example).
 */
void ConditionalAndersen::attachStaticEdgeGuards()
{
    SVFStmt::SVFStmtSetTy& phis = pag->getPTASVFStmtSet(SVFStmt::Phi);
    for (auto it = phis.begin(), eit = phis.end(); it != eit; ++it)
    {
        const PhiStmt* phi = SVFUtil::cast<PhiStmt>(*it);

        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
        {
            NodeID opId = phi->getOpVar(i)->getId();
            NodeID resId = phi->getResID();
            const ICFGNode* predNode = phi->getOpICFGNode(i);

            // predNode is the terminator of the incoming block.
            // The conditional guard is on the incoming edge to the *entry* of this block.
            if (predNode->getBB())
            {
                const ICFGNode* entryNode = predNode->getBB()->front();
                for (auto* edge : entryNode->getInEdges())
                {
                    if (const IntraCFGEdge* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
                    {
                        if (intra->getCondition())
                        {
                            bool trueBranch = (intra->getSuccessorCondValue() != 0);
                            const PathCond* guard = PathCond::getAtom(
                                                        intra->getCondition()->getId(), trueBranch);
                            staticCopyGuards[std::make_pair(opId, resId)] = guard;
                            break;
                        }
                    }
                }
            }
        }
    }
}

/*!
 * Look up the guard for a copy edge.
 * Priority: static guards > derived guards > True.
 */
const PathCond* ConditionalAndersen::getEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = staticCopyGuards.find(std::make_pair(src, dst));
    if (it != staticCopyGuards.end())
        return it->second;

    auto it2 = derivedCopyGuards.find(std::make_pair(src, dst));
    if (it2 != derivedCopyGuards.end())
        return it2->second;

    return PathCond::getTrue();
}

/*!
 * Apply k-limiting: if depth > k, collapse to True.
 */
const PathCond* ConditionalAndersen::applyKLimit(const PathCond* cond) const
{
    if (kLimit == 0) return PathCond::getTrue();
    if (cond->depth() <= kLimit) return cond;
    return PathCond::getTrue();
}

/*!
 * Override SCC merge: synchronize conditional points-to and edge guards.
 */
bool ConditionalAndersen::mergeSrcToTgt(NodeID nodeId, NodeID newRepId)
{
    if (nodeId == newRepId)
        return false;

    // 1. Save edge guards involving nodeId
    Map<std::pair<NodeID, NodeID>, const PathCond*> staticGuardsToMove;
    for (auto it = staticCopyGuards.begin(); it != staticCopyGuards.end(); )
    {
        if (it->first.first == nodeId || it->first.second == nodeId)
        {
            staticGuardsToMove[it->first] = it->second;
            it = staticCopyGuards.erase(it);
        }
        else
            ++it;
    }

    Map<std::pair<NodeID, NodeID>, const PathCond*> derivedGuardsToMove;
    for (auto it = derivedCopyGuards.begin(); it != derivedCopyGuards.end(); )
    {
        if (it->first.first == nodeId || it->first.second == nodeId)
        {
            derivedGuardsToMove[it->first] = it->second;
            it = derivedCopyGuards.erase(it);
        }
        else
            ++it;
    }

    // 2. Merge condPtsMap: move all conditional pts from sub to rep
    auto itPts = condPtsMap.find(nodeId);
    if (itPts != condPtsMap.end())
    {
        auto& repSet = condPtsMap[newRepId];
        for (const auto& pair : itPts->second)
            repSet.insert(pair);
        condPtsMap.erase(itPts);
    }

    // 3. Delegate to parent for actual edge moving / pts merging / node removal
    bool pwc = Andersen::mergeSrcToTgt(nodeId, newRepId);

    // 4. Restore edge guards with updated keys (nodeId -> newRepId)
    for (const auto& pair : staticGuardsToMove)
    {
        NodeID src = pair.first.first;
        NodeID dst = pair.first.second;
        if (src == nodeId) src = newRepId;
        if (dst == nodeId) dst = newRepId;
        staticCopyGuards[std::make_pair(src, dst)] = pair.second;
    }

    for (const auto& pair : derivedGuardsToMove)
    {
        NodeID src = pair.first.first;
        NodeID dst = pair.first.second;
        if (src == nodeId) src = newRepId;
        if (dst == nodeId) dst = newRepId;
        derivedCopyGuards[std::make_pair(src, dst)] = pair.second;
    }

    return pwc;
}

/*!
 * Process address edge: add unconditional (True, obj) to condPtsMap.
 */
void ConditionalAndersen::processAddr(const AddrCGEdge* addr)
{
    Andersen::processAddr(addr);

    NodeID dst = addr->getDstID();
    NodeID src = addr->getSrcID();

    auto& dstSet = condPtsMap[dst];
    if (dstSet.insert({PathCond::getTrue(), src}).second)
        pushIntoWorklist(dst);
}

/*!
 * Process copy edge: propagate conditional points-to with edge guard.
 */
bool ConditionalAndersen::processCopy(NodeID node, const ConstraintEdge* edge)
{
    bool parentChanged = Andersen::processCopy(node, edge);

    NodeID dst = edge->getDstID();
    const PathCond* guard = getEdgeGuard(node, dst);

    bool condChanged = false;
    auto it = condPtsMap.find(node);
    if (it != condPtsMap.end())
    {
        for (const auto& pair : it->second)
        {
            const PathCond* cond = pair.first;
            NodeID obj = pair.second;
            const PathCond* newCond = applyKLimit(PathCond::getAnd(cond, guard));

            if (!newCond->isSatisfiable()) continue;

            auto& dstSet = condPtsMap[dst];
            if (dstSet.insert({newCond, obj}).second)
                condChanged = true;
        }
    }

    if (condChanged)
        pushIntoWorklist(dst);

    return parentChanged || condChanged;
}

/*!
 * Process load edge: create derived copy edge with guard from pointer's condPts.
 */
bool ConditionalAndersen::processLoad(NodeID node, const ConstraintEdge* load)
{
    bool parentChanged = Andersen::processLoad(node, load);

    NodeID pointer = load->getSrcID();
    NodeID dst = load->getDstID();

    const PathCond* guard = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        for (const auto& pair : it->second)
        {
            if (pair.second == node)
            {
                guard = pair.first;
                break;
            }
        }
    }

    derivedCopyGuards[std::make_pair(node, dst)] = guard;
    return parentChanged;
}

/*!
 * Process store edge: create derived copy edge with guard from pointer's condPts.
 */
bool ConditionalAndersen::processStore(NodeID node, const ConstraintEdge* store)
{
    bool parentChanged = Andersen::processStore(node, store);

    NodeID src = store->getSrcID();
    NodeID pointer = store->getDstID();

    const PathCond* guard = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        for (const auto& pair : it->second)
        {
            if (pair.second == node)
            {
                guard = pair.first;
                break;
            }
        }
    }

    derivedCopyGuards[std::make_pair(src, node)] = guard;
    return parentChanged;
}

/*!
 * Alias query using conditional points-to.
 * May-alias only if there exists a common object under a satisfiable conjunction.
 */
AliasResult ConditionalAndersen::alias(NodeID v1, NodeID v2)
{
    if (v1 == v2) return MustAlias;

    const CondPointsTo& pts1 = getCondPts(v1);
    const CondPointsTo& pts2 = getCondPts(v2);

    if (pts1.empty() || pts2.empty())
        return NoAlias;

    bool mayAlias = false;
    for (const auto& p1 : pts1)
    {
        for (const auto& p2 : pts2)
        {
            if (p1.second == p2.second)
            {
                const PathCond* conj = PathCond::getAnd(p1.first, p2.first);
                if (conj->isSatisfiable())
                {
                    mayAlias = true;
                    break;
                }
            }
        }
        if (mayAlias) break;
    }

    return mayAlias ? MayAlias : NoAlias;
}

AliasResult ConditionalAndersen::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1->getId() == v2->getId()) return MustAlias;
    return alias(v1->getId(), v2->getId());
}

/*!
 * Finalize: optionally print conditional points-to for debugging (Phase 1).
 */
void ConditionalAndersen::finalize()
{
    Andersen::finalize();

    if (Options::PTSPrint())
    {
        SVFUtil::outs() << "\n========== Conditional Points-To (Phase 1) ==========\n";
        for (const auto& entry : condPtsMap)
        {
            NodeID id = entry.first;
            const SVFVar* var = pag->getGNode(id);
            if (var == nullptr) continue;

            SVFUtil::outs() << "Node " << id << " [" << var->getName() << "]: ";
            for (const auto& pair : entry.second)
            {
                NodeID obj = pair.second;
                const SVFVar* objVar = pag->getGNode(obj);
                SVFUtil::outs() << "(" << pair.first->toString() << ", "
                                << obj << "/" << (objVar ? objVar->getName() : "?") << ") ";
            }
            SVFUtil::outs() << "\n";
        }
        SVFUtil::outs() << "=====================================================\n\n";
    }
}

/*!
 * Access conditional points-to set.
 */
const ConditionalAndersen::CondPointsTo& ConditionalAndersen::getCondPts(NodeID id) const
{
    auto it = condPtsMap.find(id);
    if (it != condPtsMap.end())
        return it->second;

    static CondPointsTo empty;
    return empty;
}
