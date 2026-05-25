//===- SingleTrackCondAndersen.cpp -- Single-track conditional Andersen -----//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#include "WPA/SingleTrackCondAndersen.h"
#include "SVFIR/SVFStatements.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include "Util/Z3Expr.h"

using namespace SVF;
using namespace SVFUtil;

SingleTrackCondAndersen::SingleTrackCondAndersen(SVFIR* _pag, PTATY type)
    : Andersen(_pag, type),
      kLimit(Options::CondAnderKLimit()),
      eagerSat(Options::CondAnderEagerSat()),
      mergeCondSCC(Options::CondAnderMergeCondSCC()),
      numZ3SatChecks(0),
      numCondPtsEntries(0)
{
}

void SingleTrackCondAndersen::initialize()
{
    Andersen::initialize();
    setDetectPWC(Options::CondAnderPWC());
    attachStaticEdgeGuards();
}

void SingleTrackCondAndersen::finalize()
{
    Andersen::finalize();

    numCondPtsEntries = 0;
    for (const auto& entry : condPtsMap)
        numCondPtsEntries += entry.second.size();

    SVFUtil::outs() << "\n========== SingleTrackCondAndersen Statistics ==========\n";
    SVFUtil::outs() << "  kLimit:              " << kLimit << "\n";
    SVFUtil::outs() << "  eagerSat:            " << (eagerSat ? "true" : "false") << "\n";
    SVFUtil::outs() << "  Z3 SAT checks:       " << numZ3SatChecks << "\n";
    SVFUtil::outs() << "  CondPts entries:     " << numCondPtsEntries << "\n";
    SVFUtil::outs() << "=====================================================\n\n";
}

const PointsTo& SingleTrackCondAndersen::getPts(NodeID id)
{
    id = sccRepNode(id);
    auto it = ptsCache.find(id);
    if (it != ptsCache.end()) return it->second;
    PointsTo pts;
    auto cIt = condPtsMap.find(id);
    if (cIt != condPtsMap.end())
    {
        for (const auto& pair : cIt->second)
            if (!pair.second->isFalse())
                pts.set(pair.first);
    }
    return ptsCache[id] = std::move(pts);
}

const PointsTo& SingleTrackCondAndersen::getPts(NodeID id) const
{
    id = sccRepNode(id);
    auto it = ptsCache.find(id);
    if (it != ptsCache.end()) return it->second;
    static PointsTo emptyPts;
    emptyPts.clear();
    auto cIt = condPtsMap.find(id);
    if (cIt != condPtsMap.end())
    {
        for (const auto& pair : cIt->second)
            if (!pair.second->isFalse())
                emptyPts.set(pair.first);
    }
    return emptyPts;
}

const SingleTrackCondAndersen::CondPointsTo& SingleTrackCondAndersen::getCondPts(NodeID id) const
{
    id = sccRepNode(id);
    auto it = condPtsMap.find(id);
    if (it != condPtsMap.end()) return it->second;
    static const CondPointsTo empty;
    return empty;
}

AliasResult SingleTrackCondAndersen::alias(NodeID v1, NodeID v2)
{
    if (v1 == v2) return MustAlias;
    NodeID n1 = sccRepNode(v1);
    NodeID n2 = sccRepNode(v2);

    auto it1 = condPtsMap.find(n1);
    auto it2 = condPtsMap.find(n2);
    if (it1 == condPtsMap.end() || it2 == condPtsMap.end())
        return NoAlias;

    for (const auto& p1 : it1->second)
    {
        auto jt = it2->second.find(p1.first);
        if (jt != it2->second.end())
        {
            const PathCond* g1 = p1.second;
            const PathCond* g2 = jt->second;
            if (g1->isTrue() && g2->isTrue()) return MayAlias;
            if (z3IsSat(PathCond::getAnd(g1, g2))) return MayAlias;
        }
    }
    return NoAlias;
}

AliasResult SingleTrackCondAndersen::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1->getId() == v2->getId()) return MustAlias;
    return alias(v1->getId(), v2->getId());
}

/*!
 * Solve worklist: two-phase solving like AndersenWaveDiff.
 * Phase 1: topo-order processing of copy/gep edges.
 * Phase 2: worklist-driven load/store processing.
 */
void SingleTrackCondAndersen::solveWorklist()
{
    NodeStack& nodeStack = SCCDetect();

    while (!nodeStack.empty())
    {
        NodeID nodeId = nodeStack.top();
        nodeStack.pop();
        collapsePWCNode(nodeId);
        processNode(nodeId);
        collapseFields();
    }

    while (!isWorklistEmpty())
    {
        NodeID nodeId = popFromWorklist();
        handleLoadStorePhase2(nodeId);
        collapseFields();
    }
}

void SingleTrackCondAndersen::processNode(NodeID nodeId)
{
    if (sccRepNode(nodeId) != nodeId)
        return;
    ConstraintNode* node = consCG->getConstraintNode(nodeId);

    // Process copy edges
    for (ConstraintEdge* edge : node->getCopyOutEdges())
        processCopy(nodeId, edge);

    // Process gep edges
    for (ConstraintEdge* edge : node->getGepOutEdges())
    {
        if (GepCGEdge* gepEdge = SVFUtil::dyn_cast<GepCGEdge>(edge))
            processGep(nodeId, gepEdge);
    }

    if (mergeCondSCC)
        return;

    // For preserved SCCs, also process sub-nodes because their outgoing
    // copy/gep edges were not moved to the rep node.
    if (getSCCDetector()->GNodeSCCInfo().find(nodeId) == getSCCDetector()->GNodeSCCInfo().end())
        return;
    const NodeBS& subNodes = getSCCDetector()->subNodes(nodeId);
    if (subNodes.count() > 1)
    {
        for (NodeBS::iterator it = subNodes.begin(); it != subNodes.end(); ++it)
        {
            NodeID subId = *it;
            if (subId != nodeId && consCG->hasGNode(subId))
            {
                ConstraintNode* subNode = consCG->getGNode(subId);
                for (ConstraintEdge* edge : subNode->getCopyOutEdges())
                    processCopy(subId, edge);
                for (ConstraintEdge* edge : subNode->getGepOutEdges())
                {
                    if (GepCGEdge* gepEdge = SVFUtil::dyn_cast<GepCGEdge>(edge))
                        processGep(subId, gepEdge);
                }
            }
        }
    }
}

void SingleTrackCondAndersen::handleLoadStorePhase2(NodeID nodeId)
{
    if (sccRepNode(nodeId) != nodeId)
        return;

    // Helper lambda to process a single node's load/store edges.
    auto processOneNode = [&](NodeID nid)
    {
        ConstraintNode* n = consCG->getConstraintNode(nid);
        auto cIt = condPtsMap.find(nid);
        if (cIt != condPtsMap.end())
        {
            for (ConstraintNode::const_iterator lit = n->outgoingLoadsBegin(),
                     leit = n->outgoingLoadsEnd(); lit != leit; ++lit)
            {
                for (const auto& pair : cIt->second)
                {
                    if (processLoad(pair.first, *lit))
                        reanalyze = true;
                }
            }
            for (ConstraintNode::const_iterator sit = n->incomingStoresBegin(),
                     seit = n->incomingStoresEnd(); sit != seit; ++sit)
            {
                for (const auto& pair : cIt->second)
                {
                    if (processStore(pair.first, *sit))
                        reanalyze = true;
                }
            }
        }
    };

    processOneNode(nodeId);

    if (mergeCondSCC)
        return;

    // For preserved SCCs, also process sub-nodes.
    if (getSCCDetector()->GNodeSCCInfo().find(nodeId) == getSCCDetector()->GNodeSCCInfo().end())
        return;
    const NodeBS& subNodes = getSCCDetector()->subNodes(nodeId);
    if (subNodes.count() > 1)
    {
        for (NodeBS::iterator it = subNodes.begin(); it != subNodes.end(); ++it)
        {
            NodeID subId = *it;
            if (subId != nodeId && consCG->hasGNode(subId))
                processOneNode(subId);
        }
    }
}

void SingleTrackCondAndersen::processAddr(const AddrCGEdge* addr)
{
    numOfProcessedAddr++;
    NodeID dst = addr->getDstID();
    NodeID src = addr->getSrcID();
    if (orMergeCondPts(dst, src, PathCond::getTrue()))
    {
        invalidatePtsCache(dst);
        pushIntoWorklist(dst);
    }
}

bool SingleTrackCondAndersen::processCopy(NodeID node, const ConstraintEdge* edge)
{
    numOfProcessedCopy++;
    NodeID dst = edge->getDstID();
    const PathCond* guard = edge->getGuard();
    if (!guard) guard = PathCond::getTrue();

    auto it = condPtsMap.find(node);
    if (it == condPtsMap.end()) return false;

    bool changed = false;
    for (const auto& pair : it->second)
    {
        const PathCond* newCond = guard->isTrue() ? pair.second
                                   : PathCond::getAnd(pair.second, guard);
        if (orMergeCondPts(dst, pair.first, newCond))
        {
            changed = true;
            invalidatePtsCache(dst);
        }
    }
    if (changed)
        pushIntoWorklist(dst);
    return changed;
}

bool SingleTrackCondAndersen::processLoad(NodeID node, const ConstraintEdge* load)
{
    if (pag->isConstantObj(node) || pag->getSVFVar(load->getDstID())->isPointer() == false)
        return false;

    numOfProcessedLoad++;
    NodeID pointer = load->getSrcID();
    NodeID dst = load->getDstID();

    const PathCond* ptsG = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        auto jt = it->second.find(node);
        if (jt != it->second.end())
            ptsG = jt->second;
    }

    const PathCond* loadG = load->getGuard();
    if (!loadG) loadG = PathCond::getTrue();

    const PathCond* derivedGuard;
    if (loadG->isTrue()) derivedGuard = ptsG;
    else if (ptsG->isTrue()) derivedGuard = loadG;
    else derivedGuard = PathCond::getAnd(loadG, ptsG);

    // Create or merge a copy edge with the derived guard.
    // This is the key difference from base Andersen::processLoad which
    // creates an unconditional copy edge.
    CopyCGEdge* edge = consCG->addCopyCGEdge(node, dst, derivedGuard);
    if (!edge)
    {
        // Edge already exists; merge guard via directEdgeSet lookup.
        ConstraintNode* s = consCG->getConstraintNode(node);
        ConstraintNode* d = consCG->getConstraintNode(dst);
        ConstraintEdge keyEdge(s, d, ConstraintEdge::Copy);
        auto eit = consCG->getDirectCGEdges().find(&keyEdge);
        if (eit != consCG->getDirectCGEdges().end())
        {
            edge = SVFUtil::cast<CopyCGEdge>(*eit);
            const PathCond* oldG = edge->getGuard();
            if (oldG && !oldG->isTrue())
                edge->setGuard(PathCond::getOr(oldG, derivedGuard));
            else
                edge->setGuard(derivedGuard);
        }
    }
    return edge != nullptr;
}

bool SingleTrackCondAndersen::processStore(NodeID node, const ConstraintEdge* store)
{
    if (pag->isConstantObj(node) || pag->getSVFVar(store->getSrcID())->isPointer() == false)
        return false;

    numOfProcessedStore++;
    NodeID src = store->getSrcID();
    NodeID pointer = store->getDstID();

    const PathCond* ptsG = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        auto jt = it->second.find(node);
        if (jt != it->second.end())
            ptsG = jt->second;
    }

    const PathCond* storeG = store->getGuard();
    if (!storeG) storeG = PathCond::getTrue();

    const PathCond* derivedGuard;
    if (storeG->isTrue()) derivedGuard = ptsG;
    else if (ptsG->isTrue()) derivedGuard = storeG;
    else derivedGuard = PathCond::getAnd(storeG, ptsG);

    CopyCGEdge* edge = consCG->addCopyCGEdge(src, node, derivedGuard);
    if (!edge)
    {
        ConstraintNode* s = consCG->getConstraintNode(src);
        ConstraintNode* d = consCG->getConstraintNode(node);
        ConstraintEdge keyEdge(s, d, ConstraintEdge::Copy);
        auto eit = consCG->getDirectCGEdges().find(&keyEdge);
        if (eit != consCG->getDirectCGEdges().end())
        {
            edge = SVFUtil::cast<CopyCGEdge>(*eit);
            const PathCond* oldG = edge->getGuard();
            if (oldG && !oldG->isTrue())
                edge->setGuard(PathCond::getOr(oldG, derivedGuard));
            else
                edge->setGuard(derivedGuard);
        }
    }
    return edge != nullptr;
}

bool SingleTrackCondAndersen::processGep(NodeID, const GepCGEdge* edge)
{
    numOfProcessedGep++;
    NodeID src = edge->getSrcID();
    NodeID dst = edge->getDstID();
    const PathCond* edgeG = edge->getGuard();
    if (!edgeG) edgeG = PathCond::getTrue();

    auto it = condPtsMap.find(src);
    if (it == condPtsMap.end()) return false;

    bool changed = false;
    bool isVariant = SVFUtil::isa<VariantGepCGEdge>(edge);
    const NormalGepCGEdge* normalGep = SVFUtil::dyn_cast<NormalGepCGEdge>(edge);

    for (const auto& pair : it->second)
    {
        NodeID o = pair.first;
        NodeID newField;
        if (isVariant)
        {
            if (consCG->isBlkObjOrConstantObj(o))
            {
                newField = o;
            }
            else
            {
                if (!isFieldInsensitive(o))
                {
                    setObjFieldInsensitive(o);
                    consCG->addNodeToBeCollapsed(consCG->getBaseObjVarID(o));
                }
                newField = consCG->getFIObjVar(o);
            }
        }
        else
        {
            if (consCG->isBlkObjOrConstantObj(o) || isFieldInsensitive(o))
            {
                newField = o;
            }
            else
            {
                newField = consCG->getGepObjVar(
                    o, normalGep->getAccessPath().getConstantStructFldIdx());
            }
        }

        const PathCond* g = edgeG->isTrue() ? pair.second
                             : PathCond::getAnd(pair.second, edgeG);
        if (orMergeCondPts(dst, newField, g))
        {
            changed = true;
            invalidatePtsCache(dst);
        }
    }
    if (changed)
        pushIntoWorklist(dst);
    return changed;
}

bool SingleTrackCondAndersen::mergeSrcToTgt(NodeID nodeId, NodeID newRepId)
{
    if (nodeId == newRepId)
        return false;

    auto itPts = condPtsMap.find(nodeId);
    if (itPts != condPtsMap.end())
    {
        for (const auto& pair : itPts->second)
        {
            const PathCond* guard = mergeCondSCC ? PathCond::getTrue() : pair.second;
            orMergeCondPts(newRepId, pair.first, guard);
        }
        condPtsMap.erase(itPts);
    }

    invalidatePtsCache(nodeId);
    invalidatePtsCache(newRepId);

    bool pwc = Andersen::mergeSrcToTgt(nodeId, newRepId);
    return pwc;
}

NodeStack& SingleTrackCondAndersen::SCCDetect()
{
    numOfSCCDetection++;

    double sccStart = stat->getClk(true);
    WPAConstraintSolver::SCCDetect();
    double sccEnd = stat->getClk(true);
    timeOfSCCDetection += (sccEnd - sccStart) / TIMEINTERVAL;

    double mergeStart = stat->getClk(true);

    NodeStack topoOrder = getSCCDetector()->topoNodeStack();
    while (!topoOrder.empty())
    {
        NodeID repNodeId = topoOrder.top();
        topoOrder.pop();
        const NodeBS& subNodes = getSCCDetector()->subNodes(repNodeId);
        if (subNodes.count() <= 1)
            continue;

        bool hasCond = false;
        for (NodeBS::iterator it = subNodes.begin(), eit = subNodes.end(); it != eit; ++it)
        {
            NodeID nid = *it;
            ConstraintNode* n = consCG->getConstraintNode(nid);
            for (ConstraintEdge* e : n->getOutEdges())
            {
                if (e->getEdgeKind() == ConstraintEdge::Copy ||
                    e->getEdgeKind() == ConstraintEdge::NormalGep ||
                    e->getEdgeKind() == ConstraintEdge::VariantGep)
                {
                    const PathCond* g = e->getGuard();
                    if (g && !g->isTrue()) { hasCond = true; break; }
                }
            }
            if (hasCond) break;
            for (ConstraintEdge* e : n->getInEdges())
            {
                if (e->getEdgeKind() == ConstraintEdge::Copy ||
                    e->getEdgeKind() == ConstraintEdge::NormalGep ||
                    e->getEdgeKind() == ConstraintEdge::VariantGep)
                {
                    const PathCond* g = e->getGuard();
                    if (g && !g->isTrue()) { hasCond = true; break; }
                }
            }
            if (hasCond) break;
        }

        if (hasCond && !mergeCondSCC)
            continue;

        for (NodeBS::iterator it = subNodes.begin(), eit = subNodes.end(); it != eit; ++it)
        {
            NodeID subNodeId = *it;
            if (subNodeId != repNodeId)
                mergeNodeToRep(subNodeId, repNodeId);
        }
    }

    double mergeEnd = stat->getClk(true);
    timeOfSCCMerges += (mergeEnd - mergeStart) / TIMEINTERVAL;

    return getSCCDetector()->topoNodeStack();
}

const PathCond* SingleTrackCondAndersen::getBBGuard(const SVFBasicBlock* bb) const
{
    if (!bb) return PathCond::getTrue();
    const PathCond* g = PathCond::getFalse();
    bool hasGuard = false;
    for (const ICFGNode* entry : bb->getICFGNodeList())
    {
        for (const ICFGEdge* edge : entry->getInEdges())
        {
            if (const IntraCFGEdge* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
            {
                if (intra->getCondition())
                {
                    bool trueBranch = (intra->getSuccessorCondValue() != 0);
                    const PathCond* atom = PathCond::getAtom(
                        intra->getCondition()->getId(), trueBranch);
                    g = PathCond::getOr(g, atom);
                    hasGuard = true;
                }
            }
        }
    }
    return hasGuard ? g : PathCond::getTrue();
}

void SingleTrackCondAndersen::attachStaticEdgeGuards()
{
    auto setCopyGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        if (!guard || guard->isTrue()) return;
        CopyCGEdge* edge = consCG->addCopyCGEdge(src, dst, guard);
        if (!edge)
        {
            ConstraintNode* s = consCG->getConstraintNode(src);
            ConstraintNode* d = consCG->getConstraintNode(dst);
            ConstraintEdge keyEdge(s, d, ConstraintEdge::Copy);
            auto it = consCG->getDirectCGEdges().find(&keyEdge);
            if (it != consCG->getDirectCGEdges().end())
            {
                edge = SVFUtil::cast<CopyCGEdge>(*it);
                const PathCond* oldG = edge->getGuard();
                if (oldG && !oldG->isTrue())
                    edge->setGuard(PathCond::getOr(oldG, guard));
                else
                    edge->setGuard(guard);
            }
        }
    };

    // PhiStmt
    SVFStmt::SVFStmtSetTy& phis = pag->getPTASVFStmtSet(SVFStmt::Phi);
    for (auto it = phis.begin(), eit = phis.end(); it != eit; ++it)
    {
        const PhiStmt* phi = SVFUtil::cast<PhiStmt>(*it);
        const PathCond* phiGuard = getBBGuard(phi->getICFGNode() ? phi->getICFGNode()->getBB() : nullptr);
        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
        {
            NodeID opId = phi->getOpVar(i)->getId();
            NodeID resId = phi->getResID();
            setCopyGuard(opId, resId, phiGuard);
        }
    }

    // Plain CopyStmt
    SVFStmt::SVFStmtSetTy& copies = pag->getPTASVFStmtSet(SVFStmt::Copy);
    for (auto it = copies.begin(), eit = copies.end(); it != eit; ++it)
    {
        const CopyStmt* c = SVFUtil::cast<CopyStmt>(*it);
        const PathCond* g = getBBGuard(c->getICFGNode() ? c->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setCopyGuard(c->getRHSVarID(), c->getLHSVarID(), g);
    }

    // LoadStmt
    auto setLoadGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        if (!guard || guard->isTrue()) return;
        ConstraintNode* srcNode = consCG->getConstraintNode(src);
        for (ConstraintEdge* e : srcNode->getOutEdges())
        {
            if (e->getDstID() == dst && e->getEdgeKind() == ConstraintEdge::Load)
            {
                LoadCGEdge* edge = SVFUtil::cast<LoadCGEdge>(e);
                const PathCond* oldG = edge->getGuard();
                if (oldG && !oldG->isTrue())
                    edge->setGuard(PathCond::getOr(oldG, guard));
                else
                    edge->setGuard(guard);
                return;
            }
        }
    };
    SVFStmt::SVFStmtSetTy& loads = pag->getPTASVFStmtSet(SVFStmt::Load);
    for (auto it = loads.begin(), eit = loads.end(); it != eit; ++it)
    {
        const LoadStmt* l = SVFUtil::cast<LoadStmt>(*it);
        const PathCond* g = getBBGuard(l->getICFGNode() ? l->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setLoadGuard(l->getRHSVarID(), l->getLHSVarID(), g);
    }

    // StoreStmt
    auto setStoreGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        if (!guard || guard->isTrue()) return;
        ConstraintNode* srcNode = consCG->getConstraintNode(src);
        for (ConstraintEdge* e : srcNode->getOutEdges())
        {
            if (e->getDstID() == dst && e->getEdgeKind() == ConstraintEdge::Store)
            {
                StoreCGEdge* edge = SVFUtil::cast<StoreCGEdge>(e);
                const PathCond* oldG = edge->getGuard();
                if (oldG && !oldG->isTrue())
                    edge->setGuard(PathCond::getOr(oldG, guard));
                else
                    edge->setGuard(guard);
                return;
            }
        }
    };
    SVFStmt::SVFStmtSetTy& stores = pag->getPTASVFStmtSet(SVFStmt::Store);
    for (auto it = stores.begin(), eit = stores.end(); it != eit; ++it)
    {
        const StoreStmt* st = SVFUtil::cast<StoreStmt>(*it);
        const PathCond* g = getBBGuard(st->getICFGNode() ? st->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setStoreGuard(st->getRHSVarID(), st->getLHSVarID(), g);
    }

    // GepStmt
    auto setGepGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        if (!guard || guard->isTrue()) return;
        ConstraintNode* srcNode = consCG->getConstraintNode(src);
        for (ConstraintEdge* e : srcNode->getOutEdges())
        {
            if (e->getDstID() == dst && (e->getEdgeKind() == ConstraintEdge::NormalGep ||
                                          e->getEdgeKind() == ConstraintEdge::VariantGep))
            {
                const PathCond* oldG = e->getGuard();
                if (oldG && !oldG->isTrue())
                    e->setGuard(PathCond::getOr(oldG, guard));
                else
                    e->setGuard(guard);
                return;
            }
        }
    };
    SVFStmt::SVFStmtSetTy& geps = pag->getPTASVFStmtSet(SVFStmt::Gep);
    for (auto it = geps.begin(), eit = geps.end(); it != eit; ++it)
    {
        const GepStmt* gs = SVFUtil::cast<GepStmt>(*it);
        const PathCond* g = getBBGuard(gs->getICFGNode() ? gs->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setGepGuard(gs->getRHSVarID(), gs->getLHSVarID(), g);
    }

    // SelectStmt
    SVFStmt::SVFStmtSetTy& selects = pag->getPTASVFStmtSet(SVFStmt::Select);
    for (auto it = selects.begin(), eit = selects.end(); it != eit; ++it)
    {
        const SelectStmt* sel = SVFUtil::cast<SelectStmt>(*it);
        const SVFVar* cond = sel->getCondition();
        if (!cond) continue;
        const PathCond* condAtom = PathCond::getAtom(cond->getId(), true);
        const PathCond* negCondAtom = PathCond::getAtom(cond->getId(), false);
        const PathCond* bbG = getBBGuard(sel->getICFGNode() ? sel->getICFGNode()->getBB() : nullptr);

        if (sel->getOpVarNum() > 0)
        {
            const PathCond* trueG = bbG->isTrue() ? condAtom : PathCond::getAnd(bbG, condAtom);
            setCopyGuard(sel->getOpVar(0)->getId(), sel->getResID(), trueG);
        }
        if (sel->getOpVarNum() > 1)
        {
            const PathCond* falseG = bbG->isTrue() ? negCondAtom : PathCond::getAnd(bbG, negCondAtom);
            setCopyGuard(sel->getOpVar(1)->getId(), sel->getResID(), falseG);
        }
    }

    // CallPE
    SVFStmt::SVFStmtSetTy& callpes = pag->getPTASVFStmtSet(SVFStmt::Call);
    for (auto it = callpes.begin(), eit = callpes.end(); it != eit; ++it)
    {
        const CallPE* cpe = SVFUtil::cast<CallPE>(*it);
        for (u32_t i = 0; i < cpe->getOpVarNum(); ++i)
        {
            NodeID opId = cpe->getOpVar(i)->getId();
            NodeID resId = cpe->getResID();
            const CallICFGNode* callNode = cpe->getOpCallICFGNode(i);
            const PathCond* g = getBBGuard(callNode ? callNode->getBB() : nullptr);
            setCopyGuard(opId, resId, g);
        }
    }

    // RetPE
    SVFStmt::SVFStmtSetTy& retpes = pag->getPTASVFStmtSet(SVFStmt::Ret);
    for (auto it = retpes.begin(), eit = retpes.end(); it != eit; ++it)
    {
        const RetPE* ret = SVFUtil::cast<RetPE>(*it);
        const PathCond* g = getBBGuard(ret->getFunExitICFGNode() ? ret->getFunExitICFGNode()->getBB() : nullptr);
        setCopyGuard(ret->getRHSVarID(), ret->getLHSVarID(), g);
    }
}

bool SingleTrackCondAndersen::orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard)
{
    // In single-track mode, True guards are explicitly stored in condPtsMap
    // because condPtsMap is the sole source of truth.
    auto itOuter = condPtsMap.find(var);
    if (itOuter != condPtsMap.end())
    {
        auto& map = itOuter->second;
        auto it = map.find(obj);
        if (it != map.end())
        {
            const PathCond* merged = PathCond::getOr(it->second, guard);
            if (merged == it->second)
                return false;
            it->second = merged;
            return true;
        }
        map[obj] = guard;
        return true;
    }
    CondPointsTo newMap;
    newMap.reserve(64);
    newMap[obj] = guard;
    condPtsMap[var] = std::move(newMap);
    return true;
}

bool SingleTrackCondAndersen::z3IsSat(const PathCond* cond) const
{
    numZ3SatChecks++;
    if (cond->isTrue()) return true;
    if (cond->isFalse()) return false;

    std::function<z3::expr(const PathCond*)> build = [&](const PathCond* c) -> z3::expr {
        if (c->isTrue()) return Z3Expr::getContext().bool_val(true);
        if (c->isFalse()) return Z3Expr::getContext().bool_val(false);
        if (c->isAtom())
        {
            std::string n = "c" + std::to_string(c->getBranchId());
            z3::expr v = Z3Expr::getContext().bool_const(n.c_str());
            return c->isTrueBranch() ? v : !v;
        }
        if (c->isAnd())
            return build(c->getLeft()) && build(c->getRight());
        return build(c->getLeft()) || build(c->getRight());
    };

    z3::solver& s = Z3Expr::getSolver();
    s.push();
    s.add(build(cond));
    z3::check_result r = s.check();
    s.pop();
    return r != z3::unsat;
}
