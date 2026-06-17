//===- PathSensitiveFlowSensitive.cpp -- SPAS-style path/context/flow FS ----//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#ifdef SVF_ENABLE_SPAS

#include "WPA/PathSensitiveFlowSensitive.h"
#include "Graphs/SVFGEdge.h"
#include <algorithm>
#include <random>
#include "Graphs/VFGNode.h"
#include "MSSA/MemSSA.h"
#include "SVFIR/SVFStatements.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <cstdlib>

using namespace SVF;

std::unique_ptr<PathSensitiveFlowSensitive> PathSensitiveFlowSensitive::psfspta;

void PathSensitiveFlowSensitive::initialize()
{
    // Build the unconditional SVFG and auxiliary Andersen as FlowSensitive does.
    FlowSensitive::initialize();

    condDFPTData = new CondDFPTDataTy();
    initDirectEdgeGuards();
}

void PathSensitiveFlowSensitive::initDirectEdgeGuards()
{
    for (auto it = svfg->begin(), eit = svfg->end(); it != eit; ++it)
    {
        SVFGNode* src = it->second;
        for (SVFGEdge* edge : src->getOutEdges())
        {
            if (DirectSVFGEdge* de = SVFUtil::dyn_cast<DirectSVFGEdge>(edge))
            {
                // Call/return direct edges already carry call-site context atoms.
                if (SVFUtil::isa<CallDirSVFGEdge, RetDirSVFGEdge>(de))
                    continue;
                de->setGuard(capGuard(getNodeGuard(de->getDstNode())));
            }
        }
    }
}

void PathSensitiveFlowSensitive::finalize()
{
    if (std::getenv("SVF_DUMP_CONDPTSDATA"))
    {
        SVFUtil::outs() << "===== CondDFPTData dump =====\n";
        condDFPTData->dump();
        SVFUtil::outs() << "===== end dump =====\n";
    }

    if (Options::PsfsPrecisionSample() > 0)
        samplePrecisionGain(Options::PsfsPrecisionSample());
    if (Options::PsfsAliasSample() > 0)
        sampleAliasRefinement(Options::PsfsAliasSample());

    FlowSensitive::finalize();
}

Guard PathSensitiveFlowSensitive::getNodeGuard(const SVFGNode* node) const
{
    const ICFGNode* icfgNode = node->getICFGNode();
    if (!icfgNode)
        return Guard::getTrue();

    // The path guard of a statement is the guard of its basic block's entry,
    // because branch conditions live on inter-block ICFG edges.
    const SVFBasicBlock* bb = icfgNode->getBB();
    if (!bb)
        return Guard::getTrue();

    const ICFGNode* entry = bb->front();
    if (!entry)
        return Guard::getTrue();

    Guard g = Guard::getFalse();
    bool hasGuard = false;
    for (const auto& edge : entry->getInEdges())
    {
        if (const IntraCFGEdge* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
        {
            if (intra->getCondition())
            {
                bool trueBranch = (intra->getSuccessorCondValue() != 0);
                Guard atom = Guard::atom(Guard::PathAtom, intra->getCondition()->getId(), trueBranch);
                g = hasGuard ? (g | atom) : atom;
                hasGuard = true;
            }
        }
    }
    return hasGuard ? g : Guard::getTrue();
}

Guard PathSensitiveFlowSensitive::getEdgeGuard(const IndirectSVFGEdge* edge) const
{
    return edge->getGuard();
}

Guard PathSensitiveFlowSensitive::getFunctionContextGuard(const FunObjVar* fun, NodeID obj) const
{
    auto it = funObjContextGuard.find(fun);
    if (it == funObjContextGuard.end())
        return Guard::getTrue();
    auto jt = it->second.find(obj);
    return (jt == it->second.end()) ? Guard::getTrue() : jt->second;
}

Guard PathSensitiveFlowSensitive::getFunctionContextGuard(const FunObjVar* fun) const
{
    auto it = funObjContextGuard.find(fun);
    if (it == funObjContextGuard.end())
        return Guard::getTrue();
    Guard g = Guard::getFalse();
    for (const auto& pair : it->second)
        g = g | pair.second;
    return g;
}

void PathSensitiveFlowSensitive::absorbContextAtom(const FunObjVar* fun, NodeID obj, const Guard& ctx)
{
    if (ctx.isTrue() || ctx.isFalse())
        return;
    Guard& g = funObjContextGuard[fun][obj];
    g = g | ctx;
}

Guard PathSensitiveFlowSensitive::getPhiOperandGuard(const PHISVFGNode* phi, u32_t pos) const
{
    const ICFGNode* icfgNode = phi->getICFGNode();
    if (!icfgNode)
        return Guard::getTrue();
    const SVFBasicBlock* bb = icfgNode->getBB();
    if (!bb)
        return Guard::getTrue();

    const std::vector<const SVFBasicBlock*>& preds = bb->getPredecessors();
    if (pos >= preds.size())
        return Guard::getTrue();

    const SVFBasicBlock* pred = preds[pos];
    const ICFGNode* predExit = pred->getICFGNodeList().back();
    const ICFGNode* phiEntry = bb->front();

    for (const auto& edge : phiEntry->getInEdges())
    {
        if (edge->getSrcNode() != predExit)
            continue;
        if (const IntraCFGEdge* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
        {
            if (intra->getCondition())
            {
                bool trueBranch = (intra->getSuccessorCondValue() != 0);
                return Guard::atom(Guard::PathAtom, intra->getCondition()->getId(), trueBranch);
            }
        }
    }
    return Guard::getTrue();
}

PointsTo PathSensitiveFlowSensitive::getOverallCondPts(CondDFPTDataTy::LocID loc, NodeID var) const
{
    PointsTo overall;
    for (const auto& pair : condDFPTData->getCondOutPtsSet(loc, var))
        overall |= pair.second;
    for (const auto& pair : condDFPTData->getCondInPtsSet(loc, var))
        overall |= pair.second;
    return overall;
}

bool PathSensitiveFlowSensitive::processSVFGNode(SVFGNode* node)
{
    double start = stat->getClk();
    bool changed = false;

    if (AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(node))
    {
        numOfProcessedAddr++;
        if (processAddr(addr))
            changed = true;
    }
    else if (CopySVFGNode* copy = SVFUtil::dyn_cast<CopySVFGNode>(node))
    {
        numOfProcessedCopy++;
        if (processCopy(copy))
            changed = true;
    }
    else if (GepSVFGNode* gep = SVFUtil::dyn_cast<GepSVFGNode>(node))
    {
        numOfProcessedGep++;
        if (processGep(gep))
            changed = true;
    }
    else if (LoadSVFGNode* load = SVFUtil::dyn_cast<LoadSVFGNode>(node))
    {
        numOfProcessedLoad++;
        if (processLoad(load))
            changed = true;
    }
    else if (StoreSVFGNode* store = SVFUtil::dyn_cast<StoreSVFGNode>(node))
    {
        numOfProcessedStore++;
        if (processStore(store))
            changed = true;
    }
    else if (PHISVFGNode* phi = SVFUtil::dyn_cast<PHISVFGNode>(node))
    {
        numOfProcessedPhi++;
        if (processPhi(phi))
            changed = true;
    }
    else if (SVFUtil::isa<MSSAPHISVFGNode, FormalINSVFGNode,
             FormalOUTSVFGNode, ActualINSVFGNode,
             ActualOUTSVFGNode>(node))
    {
        numOfProcessedMSSANode++;
        changed = true;
    }
    else if (const ActualParmSVFGNode* ap = SVFUtil::dyn_cast<ActualParmSVFGNode>(node))
    {
        NodeID var = ap->getParam()->getId();
        if (condDFPTData->updateDFOutFromIn(node->getId(), var, node->getId(), var, Guard::getTrue()))
            changed = true;
        changed = true;
    }
    else if (const FormalParmSVFGNode* fp = SVFUtil::dyn_cast<FormalParmSVFGNode>(node))
    {
        NodeID var = fp->getParam()->getId();
        if (condDFPTData->updateDFOutFromIn(node->getId(), var, node->getId(), var, Guard::getTrue()))
            changed = true;
        changed = true;
    }
    else if (const FormalRetSVFGNode* fr = SVFUtil::dyn_cast<FormalRetSVFGNode>(node))
    {
        NodeID var = fr->getRet()->getId();
        if (condDFPTData->updateDFOutFromIn(node->getId(), var, node->getId(), var, Guard::getTrue()))
            changed = true;
        changed = true;
    }
    else if (const ActualRetSVFGNode* ar = SVFUtil::dyn_cast<ActualRetSVFGNode>(node))
    {
        NodeID var = ar->getRev()->getId();
        if (condDFPTData->updateDFOutFromIn(node->getId(), var, node->getId(), var, Guard::getTrue()))
            changed = true;
        changed = true;
    }
    else if (SVFUtil::isa<NullPtrSVFGNode>(node))
    {
        changed = true;
    }
    else if (SVFUtil::isa<CmpVFGNode, BinaryOPVFGNode>(node) ||
             SVFUtil::dyn_cast<UnaryOPVFGNode>(node))
    {
    }
    else
    {
        assert(false && "unexpected kind of SVFG nodes");
    }

    double end = stat->getClk();
    processTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processAddr(const AddrSVFGNode* addr)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID dstVar = addr->getDstNodeID();
    NodeID obj = addr->getSrcNodeID();
    if (isFieldInsensitive(obj))
        obj = getFIObjVar(obj);

    PointsTo pts;
    pts.set(obj);
    if (condDFPTData->unionCondOutPts(addr->getId(), dstVar, Guard::getTrue(), pts))
        changed = true;
    if (addPts(dstVar, obj))
        changed = true;

    double end = stat->getClk();
    addrTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processCopy(const CopySVFGNode* copy)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID srcVar = copy->getSrcNodeID();
    NodeID dstVar = copy->getDstNodeID();
    Guard nodeGuard = capGuard(getNodeGuard(copy));

    PointsTo overall;
    const CondDFPTDataTy::GuardPtsMap& inMap = condDFPTData->getCondInPtsSet(copy->getId(), srcVar);
    for (const auto& pair : inMap)
    {
        Guard outGuard = capGuard(pair.first & nodeGuard);
        if (!outGuard.isSat())
            continue;
        if (condDFPTData->unionCondOutPts(copy->getId(), dstVar, outGuard, pair.second))
            changed = true;
        overall |= pair.second;
    }

    if (!overall.empty())
    {
        if (unionPts(dstVar, overall))
            changed = true;
    }

    double end = stat->getClk();
    copyTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processGep(const GepSVFGNode* gep)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID srcVar = gep->getSrcNodeID();
    NodeID dstVar = gep->getDstNodeID();
    const GepStmt* gepStmt = SVFUtil::cast<GepStmt>(gep->getSVFStmt());

    PointsTo overall;
    const CondDFPTDataTy::GuardPtsMap& inMap = condDFPTData->getCondInPtsSet(gep->getId(), srcVar);
    for (const auto& pair : inMap)
    {
        PointsTo tmpDstPts;
        if (gepStmt->isVariantFieldGep())
        {
            for (NodeID o : pair.second)
            {
                if (isBlkObjOrConstantObj(o))
                {
                    tmpDstPts.set(o);
                    continue;
                }
                setObjFieldInsensitive(o);
                tmpDstPts.set(getFIObjVar(o));
            }
        }
        else
        {
            for (NodeID o : pair.second)
            {
                if (isBlkObjOrConstantObj(o) || isFieldInsensitive(o))
                {
                    tmpDstPts.set(o);
                    continue;
                }
                NodeID field = getGepObjVar(o, gepStmt->getAccessPath().getConstantStructFldIdx());
                tmpDstPts.set(field);
            }
        }

        if (!tmpDstPts.empty())
        {
            Guard outGuard = capGuard(pair.first & capGuard(getNodeGuard(gep)));
            if (outGuard.isSat() && condDFPTData->unionCondOutPts(gep->getId(), dstVar, outGuard, tmpDstPts))
                changed = true;
            overall |= tmpDstPts;
        }
    }

    if (!overall.empty())
    {
        if (unionPts(dstVar, overall))
            changed = true;
    }

    double end = stat->getClk();
    gepTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processPhi(const PHISVFGNode* phi)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID resVar = phi->getRes()->getId();
    PointsTo overall;

    for (PHISVFGNode::OPVers::const_iterator it = phi->opVerBegin(), eit = phi->opVerEnd();
            it != eit; ++it)
    {
        u32_t pos = it->first;
        NodeID srcVar = it->second->getId();
        Guard edgeGuard = getPhiOperandGuard(phi, pos);

        const CondDFPTDataTy::GuardPtsMap& inMap = condDFPTData->getCondInPtsSet(phi->getId(), srcVar);
        for (const auto& pair : inMap)
        {
            Guard combined = capGuard(pair.first & edgeGuard & capGuard(getNodeGuard(phi)));
            if (!combined.isSat())
                continue;
            if (condDFPTData->unionCondOutPts(phi->getId(), resVar, combined, pair.second))
                changed = true;
            overall |= pair.second;
        }
    }

    if (!overall.empty())
    {
        if (unionPts(resVar, overall))
            changed = true;
    }

    double end = stat->getClk();
    phiTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processLoad(const LoadSVFGNode* load)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID dstVar = load->getDstNodeID();
    NodeID srcVar = load->getSrcNodeID();

    if (!load->getDstNode()->isPointer())
    {
        double end = stat->getClk();
        loadTime += (end - start) / TIMEINTERVAL;
        return false;
    }

    Guard loadGuard = capGuard(getNodeGuard(load));
    PointsTo overall;

    auto readObject = [&](NodeID obj, const Guard& objGuard)
    {
        bool localChanged = false;
        if (pag->isConstantObj(obj))
            return localChanged;

        const CondDFPTDataTy::GuardPtsMap& memMap = condDFPTData->getCondInPtsSet(load->getId(), obj);
        for (const auto& pair : memMap)
        {
            Guard combined = capGuard(pair.first & objGuard & loadGuard);
            if (!combined.isSat())
                continue;
            if (condDFPTData->unionCondOutPts(load->getId(), dstVar, combined, pair.second))
                localChanged = true;
            overall |= pair.second;
        }

        if (isFieldInsensitive(obj))
        {
            const NodeBS& allFields = getAllFieldsObjVars(obj);
            for (NodeID field : allFields)
            {
                const CondDFPTDataTy::GuardPtsMap& fieldMap = condDFPTData->getCondInPtsSet(load->getId(), field);
                for (const auto& pair : fieldMap)
                {
                    Guard combined = capGuard(pair.first & objGuard & loadGuard);
                    if (!combined.isSat())
                        continue;
                    if (condDFPTData->unionCondOutPts(load->getId(), dstVar, combined, pair.second))
                        localChanged = true;
                    overall |= pair.second;
                }
            }
        }
        return localChanged;
    };

    const CondDFPTDataTy::GuardPtsMap& srcMap = condDFPTData->getCondInPtsSet(load->getId(), srcVar);
    if (!srcMap.empty())
    {
        for (const auto& pair : srcMap)
        {
            for (NodeID obj : pair.second)
                if (readObject(obj, pair.first))
                    changed = true;
        }
    }
    else
    {
        // Fallback: the source pointer's conditional IN set has not been populated yet.
        for (NodeID ptd : getPts(srcVar))
            if (readObject(ptd, Guard::getTrue()))
                changed = true;
    }

    if (!overall.empty())
    {
        if (unionPts(dstVar, overall))
            changed = true;
    }

    double end = stat->getClk();
    loadTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::processStore(const StoreSVFGNode* store)
{
    double start = stat->getClk();
    bool changed = false;

    NodeID srcVar = store->getSrcNodeID();
    NodeID dstVar = store->getDstNodeID();

    // Memory state after the store starts from the state before the store.
    {
        CondDFPTDataTy::CondPtsMap& inMap = condDFPTData->getDFInPtsMap(store->getId());
        for (auto& varPair : inMap)
        {
            NodeID var = varPair.first;
            for (auto& guardPair : varPair.second)
            {
                if (condDFPTData->unionCondOutPts(store->getId(), var, guardPair.first, guardPair.second))
                    changed = true;
            }
        }
    }

    Guard branchGuard = capGuard(getNodeGuard(store));
    const FunObjVar* fun = store->getICFGNode() && store->getICFGNode()->getBB()
                            ? store->getICFGNode()->getBB()->getParent()
                            : nullptr;

    auto updateTarget = [&](NodeID obj, const Guard& objGuard, bool forceSU)
    {
        if (pag->isConstantObj(obj))
            return;

        Guard ctxGuard = fun ? getFunctionContextGuard(fun, obj) : Guard::getTrue();
        Guard storeGuard = capGuard(branchGuard & ctxGuard);

        const CondDFPTDataTy::GuardPtsMap& srcMap = condDFPTData->getCondInPtsSet(store->getId(), srcVar);
        if (!srcMap.empty())
        {
            for (const auto& pair : srcMap)
            {
                Guard combined = capGuard(pair.first & objGuard & storeGuard);
                if (!combined.isSat())
                    continue;
                applyStoreUpdate(store->getId(), obj, combined, pair.second, forceSU);
            }
        }
        else if (store->getSrcNode()->isPointer())
        {
            Guard combined = capGuard(objGuard & storeGuard);
            if (combined.isSat())
                applyStoreUpdate(store->getId(), obj, combined, getPts(srcVar), forceSU);
        }
    };

    const CondDFPTDataTy::GuardPtsMap& dstMap = condDFPTData->getCondInPtsSet(store->getId(), dstVar);
    if (!dstMap.empty())
    {
        for (const auto& pair : dstMap)
        {
            bool guardSingleton = (pair.second.count() == 1);
            for (NodeID obj : pair.second)
            {
                bool forceSU = guardSingleton && isStrongUpdatableObject(obj);
                updateTarget(obj, pair.first, forceSU);
                if (isFieldInsensitive(obj))
                {
                    const NodeBS& allFields = getAllFieldsObjVars(obj);
                    for (NodeID field : allFields)
                        updateTarget(field, pair.first, false);
                }
            }
        }
    }
    else
    {
        for (NodeID obj : getPts(dstVar))
        {
            updateTarget(obj, Guard::getTrue(), false);
            if (isFieldInsensitive(obj))
            {
                const NodeBS& allFields = getAllFieldsObjVars(obj);
                for (NodeID field : allFields)
                    updateTarget(field, Guard::getTrue(), false);
            }
        }
    }

    double end = stat->getClk();
    storeTime += (end - start) / TIMEINTERVAL;
    return changed;
}

void PathSensitiveFlowSensitive::applyStoreUpdate(CondDFPTDataTy::LocID loc, NodeID obj,
        const Guard& guard, const PointsTo& pts, bool strongUpdate)
{
    if (strongUpdate)
    {
        CondDFPTDataTy::GuardPtsMap& outMap = condDFPTData->getCondOutPtsSet(loc, obj);
        std::vector<Guard> toErase;
        std::vector<std::pair<Guard, PointsTo>> toAdd;
        for (const auto& gp : outMap)
        {
            Guard overlap = gp.first & guard;
            if (!overlap.isSat())
                continue;

            Guard residual = gp.first & !guard;
            if (residual.isSat())
                toAdd.emplace_back(residual, gp.second);

            toErase.push_back(gp.first);
        }
        for (const Guard& g : toErase)
            outMap.erase(g);
        for (const auto& p : toAdd)
            condDFPTData->unionCondOutPts(loc, obj, p.first, p.second);

        condDFPTData->unionCondOutPts(loc, obj, guard, pts);
    }
    else
    {
        condDFPTData->unionCondOutPts(loc, obj, guard, pts);
    }
}

bool PathSensitiveFlowSensitive::propAlongDirectEdge(const DirectSVFGEdge* edge)
{
    double start = stat->getClk();
    bool changed = false;

    SVFGNode* src = edge->getSrcNode();
    SVFGNode* dst = edge->getDstNode();
    Guard edgeGuard = capGuard(edge->getGuard());

    // Context-sensitive parameter/return edges propagate only the single
    // argument/return variable and carry the call-site context guard.
    if (const CallDirSVFGEdge* callEdge = SVFUtil::dyn_cast<CallDirSVFGEdge>(edge))
    {
        (void)callEdge;
        const ActualParmSVFGNode* ap = SVFUtil::cast<ActualParmSVFGNode>(src);
        const FormalParmSVFGNode* fp = SVFUtil::cast<FormalParmSVFGNode>(dst);
        NodeID srcVar = ap->getParam()->getId();
        NodeID dstVar = fp->getParam()->getId();
        if (condDFPTData->updateDFInFromIn(src->getId(), srcVar, dst->getId(), dstVar, edgeGuard))
            changed = true;
    }
    else if (const RetDirSVFGEdge* retEdge = SVFUtil::dyn_cast<RetDirSVFGEdge>(edge))
    {
        (void)retEdge;
        const FormalRetSVFGNode* fr = SVFUtil::cast<FormalRetSVFGNode>(src);
        const ActualRetSVFGNode* ar = SVFUtil::cast<ActualRetSVFGNode>(dst);
        NodeID srcVar = fr->getRet()->getId();
        NodeID dstVar = ar->getRev()->getId();
        if (condDFPTData->updateDFInFromIn(src->getId(), srcVar, dst->getId(), dstVar, edgeGuard))
            changed = true;
    }
    else
    {
        // Intra-procedural direct edges carry the whole top-level value map from
        // the source definition to the destination use.
        if (condDFPTData->updateAllDFInFromOut(src->getId(), dst->getId(), edgeGuard))
            changed = true;
    }

    double end = stat->getClk();
    directPropaTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::propAlongIndirectEdge(const IndirectSVFGEdge* edge)
{
    double start = stat->getClk();

    SVFGNode* src = edge->getSrcNode();
    SVFGNode* dst = edge->getDstNode();
    Guard edgeGuard = capGuard(getEdgeGuard(edge));
    const bool isReturnEdge = SVFUtil::isa<RetIndSVFGEdge>(edge);

    bool changed = false;

    const NodeBS& pts = edge->getPointsTo();
    for (NodeID ptd : pts)
    {
        auto propagateOne = [&](NodeID var)
        {
            bool localChanged = false;
            if (SVFUtil::isa<StoreSVFGNode>(src))
            {
                localChanged = condDFPTData->updateDFInFromOut(src->getId(), var, dst->getId(), var, edgeGuard);
            }
            else if (isReturnEdge)
            {
                const CondDFPTDataTy::GuardPtsMap& inMap = condDFPTData->getCondInPtsSet(src->getId(), var);
                for (const auto& pair : inMap)
                {
                    const Guard& g = pair.first;
                    const PointsTo& ptsSet = pair.second;
                    if (g.implies(edgeGuard))
                    {
                        if (condDFPTData->unionCondInPts(dst->getId(), var, g, ptsSet))
                            localChanged = true;
                    }
                    else if (g.isContextIndependent())
                    {
                        Guard tagged = capGuard(g & edgeGuard);
                        if (tagged.isSat() && condDFPTData->unionCondInPts(dst->getId(), var, tagged, ptsSet))
                            localChanged = true;
                    }
                }
            }
            else
            {
                localChanged = condDFPTData->updateDFInFromIn(src->getId(), var, dst->getId(), var, edgeGuard);
            }
            return localChanged;
        };

        if (propagateOne(ptd))
            changed = true;

        if (isFieldInsensitive(ptd))
        {
            const NodeBS& allFields = getAllFieldsObjVars(ptd);
            for (NodeID field : allFields)
                if (propagateOne(field))
                    changed = true;
        }
    }

    if (const CallIndSVFGEdge* callEdge = SVFUtil::dyn_cast<CallIndSVFGEdge>(edge))
    {
        if (const FormalINSVFGNode* fIn = SVFUtil::dyn_cast<FormalINSVFGNode>(dst))
        {
            const FunObjVar* fun = fIn->getFun();
            NodeID csId = callEdge->getCallSiteId();
            callSiteToCallee[csId] = fun;
            funCallSites[fun].insert(csId);
            for (NodeID ptd : pts)
                absorbContextAtom(fun, ptd, edgeGuard);
        }
    }

    double end = stat->getClk();
    indirectPropaTime += (end - start) / TIMEINTERVAL;
    return changed;
}

bool PathSensitiveFlowSensitive::propVarPtsFromSrcToDst(NodeID var, const SVFGNode* src, const SVFGNode* dst)
{
    return FlowSensitive::propVarPtsFromSrcToDst(var, src, dst);
}

AliasResult PathSensitiveFlowSensitive::alias(NodeID node1, NodeID node2)
{
    // First try location-sensitive alias: both pointers must have a fact at the
    // same program point (OUT location).  This avoids false positives from
    // combining points-to facts that live at different, mutually-unreachable
    // definitions.
    Set<CondDFPTDataTy::LocID> locs1;
    Set<CondDFPTDataTy::LocID> locs2;
    condDFPTData->collectLocationsForVar(node1, locs1);
    condDFPTData->collectLocationsForVar(node2, locs2);

    auto mayAliasUnderGuard = [&](const Guard& g1, const PointsTo& s1,
                                   const Guard& g2, const PointsTo& s2) -> bool
    {
        Guard base = g1 & g2;
        Guard combined = base & getAliasContextConstraint(base);
        if (!combined.isSat())
            return false;
        PointsTo inter = s1;
        inter &= s2;
        if (inter.empty())
            return false;
        expandFIObjs(inter, inter);
        if (inter.test(0) || inter.test(pag->getBlkPtr()))
            return true;
        for (NodeID o : inter)
        {
            (void)o;
            return true;
        }
        return false;
    };

    for (CondDFPTDataTy::LocID loc : locs1)
    {
        if (!locs2.count(loc))
            continue;

        const CondDFPTDataTy::GuardPtsMap& pts1 = condDFPTData->getCondOutPtsSet(loc, node1);
        const CondDFPTDataTy::GuardPtsMap& pts2 = condDFPTData->getCondOutPtsSet(loc, node2);
        for (const auto& p1 : pts1)
        {
            for (const auto& p2 : pts2)
            {
                if (mayAliasUnderGuard(p1.first, p1.second, p2.first, p2.second))
                    return MayAlias;
            }
        }
    }

    // If no common location proves alias, fall back to the global (and less
    // precise) cross-location check for soundness.  This only happens when one
    // or both variables have no conditional OUT facts at shared program points.
    std::vector<std::pair<Guard, PointsTo>> pts1;
    std::vector<std::pair<Guard, PointsTo>> pts2;
    condDFPTData->collectAllOutPtsForVar(node1, pts1);
    condDFPTData->collectAllOutPtsForVar(node2, pts2);

    if (pts1.empty() || pts2.empty())
        return FlowSensitive::alias(node1, node2);

    for (const auto& p1 : pts1)
    {
        for (const auto& p2 : pts2)
        {
            if (mayAliasUnderGuard(p1.first, p1.second, p2.first, p2.second))
                return MayAlias;
        }
    }

    return NoAlias;
}

PointsTo PathSensitiveFlowSensitive::getOverallCondPtsAllLocations(NodeID var) const
{
    PointsTo overall;
    Set<CondDFPTDataTy::LocID> locs;
    condDFPTData->collectLocationsForVar(var, locs);
    for (CondDFPTDataTy::LocID loc : locs)
    {
        for (const auto& pair : condDFPTData->getCondOutPtsSet(loc, var))
            overall |= pair.second;
    }
    return overall;
}

void PathSensitiveFlowSensitive::samplePrecisionGain(u32_t sampleSize)
{
    SVFUtil::outs() << "  [psfs-sample-precision] Collecting top-level pointers...\n";
    std::vector<NodeID> topVars;
    for (const auto& p : pag->getSVFVarMap())
    {
        if (pag->isValidTopLevelPtr(p.second))
            topVars.push_back(p.first);
    }

    std::sort(topVars.begin(), topVars.end());
    std::mt19937 rng(42);
    if (topVars.size() > sampleSize)
    {
        std::shuffle(topVars.begin(), topVars.end(), rng);
        topVars.resize(sampleSize);
    }

    u32_t sampled = static_cast<u32_t>(topVars.size());
    SVFUtil::outs() << "  [psfs-sample-precision] Sampled " << sampled << " pointers.\n";

    if (!ander)
    {
        SVFUtil::outs() << "  [psfs-sample-precision] No Andersen baseline, skipping.\n";
        return;
    }

    u64_t totalAnder = 0;
    u64_t totalPsfs = 0;
    u64_t refinedCount = 0;
    u64_t totalReduction = 0;
    u64_t maxReduction = 0;

    for (NodeID v : topVars)
    {
        size_t anderSize = ander->getPts(v).count();
        PointsTo psfsPts = getOverallCondPtsAllLocations(v);
        size_t psfsSize = psfsPts.count();

        totalAnder += anderSize;
        totalPsfs += psfsSize;

        if (psfsSize < anderSize)
        {
            refinedCount++;
            u64_t reduction = static_cast<u64_t>(anderSize - psfsSize);
            totalReduction += reduction;
            if (reduction > maxReduction)
                maxReduction = reduction;
        }
    }

    double avgReduction = (refinedCount > 0)
                          ? (static_cast<double>(totalReduction) / static_cast<double>(refinedCount))
                          : 0.0;

    SVFUtil::outs() << "  [psfs-sample-precision] totalAndersen=" << totalAnder
                    << " totalPsfs=" << totalPsfs
                    << " refined=" << refinedCount
                    << " totalReduction=" << totalReduction
                    << " avgReduction=" << avgReduction
                    << " maxReduction=" << maxReduction
                    << "\n";
}

void PathSensitiveFlowSensitive::sampleAliasRefinement(u32_t sampleSize)
{
    SVFUtil::outs() << "  [psfs-sample-alias] Collecting top-level pointers...\n";
    std::vector<NodeID> topVars;
    for (const auto& p : pag->getSVFVarMap())
    {
        if (pag->isValidTopLevelPtr(p.second))
            topVars.push_back(p.first);
    }

    if (topVars.size() < 2 || !ander)
    {
        SVFUtil::outs() << "  [psfs-sample-alias] Not enough pointers or no Andersen baseline, skipping.\n";
        return;
    }

    std::sort(topVars.begin(), topVars.end());
    std::mt19937 rng(42);

    u32_t mayAliasAnder = 0;
    u32_t refinedNoAlias = 0;
    u32_t checked = 0;

    // Sample pairs uniformly without pre-generating all pairs for large programs.
    std::uniform_int_distribution<size_t> dist(0, topVars.size() - 1);
    while (checked < sampleSize)
    {
        NodeID a = topVars[dist(rng)];
        NodeID b = topVars[dist(rng)];
        if (a == b)
            continue;
        if (a > b)
            std::swap(a, b);
        checked++;

        AliasResult ar = ander->alias(a, b);
        if (ar != MayAlias)
            continue;
        mayAliasAnder++;

        if (alias(a, b) == NoAlias)
            refinedNoAlias++;
    }

    SVFUtil::outs() << "  [psfs-sample-alias] checked=" << checked
                    << " anderMayAlias=" << mayAliasAnder
                    << " refinedToNoAlias=" << refinedNoAlias
                    << "\n";
}

Guard PathSensitiveFlowSensitive::capGuard(const Guard& g) const
{
    if (!useDepthLimit || kLimit < 0)
        return g;
    if (g.isTrue() || g.isFalse())
        return g;
    if (g.supportSize() > static_cast<int>(kLimit))
        return Guard::getTrue();
    return g;
}

bool PathSensitiveFlowSensitive::isStrongUpdatableObject(NodeID obj) const
{
    if (pag->isConstantObj(obj) || isHeapMemObj(obj) || isArrayMemObj(obj))
        return false;
    const BaseObjVar* baseObj = pag->getBaseObject(obj);
    if (baseObj && baseObj->isFieldInsensitive())
        return false;
    if (isLocalVarInRecursiveFun(obj))
        return false;
    return true;
}

void PathSensitiveFlowSensitive::solveOnce()
{
    FlowSensitive::solveConstraints();
}

void PathSensitiveFlowSensitive::computeFunctionSummary(s32_t k)
{
    Map<NodeID, CondDFPTDataTy::CondPtsMap>& memSummary = funMemSummaryByK[k];
    Map<NodeID, CondDFPTDataTy::CondPtsMap>& retSummary = funRetSummaryByK[k];
    memSummary.clear();
    retSummary.clear();

    for (auto it = svfg->begin(), eit = svfg->end(); it != eit; ++it)
    {
        SVFGNode* node = it->second;
        if (FormalOUTSVFGNode* fOut = SVFUtil::dyn_cast<FormalOUTSVFGNode>(node))
        {
            NodeID loc = fOut->getId();
            for (NodeID obj : fOut->getPointsTo())
            {
                const CondDFPTDataTy::GuardPtsMap& pts = condDFPTData->getCondOutPtsSet(loc, obj);
                if (!pts.empty())
                    memSummary[loc][obj] = pts;
            }
        }
        else if (FormalRetSVFGNode* fRet = SVFUtil::dyn_cast<FormalRetSVFGNode>(node))
        {
            NodeID loc = fRet->getId();
            NodeID retVar = fRet->getRet()->getId();
            const CondDFPTDataTy::GuardPtsMap& pts = condDFPTData->getCondOutPtsSet(loc, retVar);
            if (!pts.empty())
                retSummary[loc][retVar] = pts;
        }
    }

    buildContextExclusivityConstraints();
}

void PathSensitiveFlowSensitive::seedFunctionSummary(s32_t k)
{
    auto memIt = funMemSummaryByK.find(k);
    if (memIt != funMemSummaryByK.end())
    {
        for (const auto& locPair : memIt->second)
        {
            NodeID loc = locPair.first;
            for (const auto& varPair : locPair.second)
            {
                NodeID obj = varPair.first;
                for (const auto& guardPair : varPair.second)
                    condDFPTData->unionCondInPts(loc, obj, guardPair.first, guardPair.second);
            }
        }
    }

    auto retIt = funRetSummaryByK.find(k);
    if (retIt != funRetSummaryByK.end())
    {
        for (const auto& locPair : retIt->second)
        {
            NodeID loc = locPair.first;
            for (const auto& varPair : locPair.second)
            {
                NodeID retVar = varPair.first;
                for (const auto& guardPair : varPair.second)
                    condDFPTData->unionCondOutPts(loc, retVar, guardPair.first, guardPair.second);
            }
        }
    }
}

void PathSensitiveFlowSensitive::buildContextExclusivityConstraints()
{
    funContextExclusivity.clear();
    for (const auto& pair : funCallSites)
    {
        const FunObjVar* fun = pair.first;
        const Set<NodeID>& sites = pair.second;
        if (sites.size() <= 1)
            continue;

        Guard constraint = Guard::getTrue();
        for (auto it1 = sites.begin(); it1 != sites.end(); ++it1)
        {
            auto it2 = it1;
            ++it2;
            for (; it2 != sites.end(); ++it2)
            {
                Guard a1 = Guard::atom(Guard::ContextAtom, *it1, true);
                Guard a2 = Guard::atom(Guard::ContextAtom, *it2, true);
                constraint = constraint & ((!a1) | (!a2));
            }
        }
        funContextExclusivity[fun] = constraint;
    }
}

Guard PathSensitiveFlowSensitive::getAliasContextConstraint(const Guard& g) const
{
    Guard constraint = Guard::getTrue();
    for (const auto& pair : funCallSites)
    {
        const FunObjVar* fun = pair.first;
        const Set<NodeID>& sites = pair.second;

        Guard disj = Guard::getFalse();
        for (NodeID cs : sites)
            disj = disj | Guard::atom(Guard::ContextAtom, cs, true);

        if ((g & disj).isSat())
        {
            auto it = funContextExclusivity.find(fun);
            if (it != funContextExclusivity.end())
                constraint = constraint & it->second;
        }
    }
    return constraint;
}

void PathSensitiveFlowSensitive::solveConstraints()
{
    if (!useRefinement || kLimit <= 0)
    {
        currentK = kLimit;
        solveOnce();
        if (useRefinement)
            computeFunctionSummary(currentK);
        return;
    }

    // Level-by-level refinement: run with k=1,2,...,kLimit.
    // The conditional store is reset each iteration; the unconditional fallback ptD
    // is kept monotonic so call-graph refinement remains sound.
    for (s32_t k = 1; k <= kLimit; ++k)
    {
        currentK = k;
        condDFPTData->clear();
        funObjContextGuard.clear();
        if (k > 1)
            seedFunctionSummary(k - 1);
        SVFUtil::outs() << "[psfs] refinement iteration k=" << k << "\n";

        double start = stat->getClk(true);
        solveOnce();
        double end = stat->getClk(true);
        double iterTime = (end - start) / TIMEINTERVAL;
        SVFUtil::outs() << "[psfs] k=" << k
                        << " time=" << iterTime << "s"
                        << " inEntries=" << condDFPTData->numInEntries()
                        << " outEntries=" << condDFPTData->numOutEntries()
                        << "\n";

        computeFunctionSummary(k);
    }
}

#endif // SVF_ENABLE_SPAS
