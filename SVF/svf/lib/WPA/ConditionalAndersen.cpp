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

ConditionalAndersen::ConditionalAndersen(SVFIR* _pag, PTATY type)
    : Andersen(_pag, type),
      kLimit(Options::CondAnderKLimit()),
      eagerSat(Options::CondAnderEagerSat()),
      useFastGuard(Options::CondAnderFastGuard()),
      numZ3SatChecks(0),
      numAliasRefined(0),
      numAliasTotal(0),
      numCondPtsEntries(0)
{
}

/*!
 * Initialize analysis.
 * Disable differential points-to to ensure processCopy is always invoked.
 */
void ConditionalAndersen::initialize()
{
    Andersen::initialize();
    setDetectPWC(Options::CondAnderPWC());
    attachStaticEdgeGuards();
}

/*!
 * Attach path-condition guards to static copy edges derived from PhiStmt
 * and plain CopyStmt.
 *
 * For each statement we compute the path guard of its enclosing basic block
 * (the conjunction of all branch conditions on edges entering the block).
 * If a block has multiple conditional predecessors their guards are OR-merged.
 */
const PathCond* ConditionalAndersen::getBBGuard(const SVFBasicBlock* bb) const
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

void ConditionalAndersen::attachStaticEdgeGuards()
{
    auto setCopyGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic});
        if (it == edgeGuards.end())
            edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic}] = guard;
        else
            it->second = PathCond::getOr(it->second, guard);
    };

    // --- PhiStmt: per-operand guard from operand's incoming edge ---
    SVFStmt::SVFStmtSetTy& phis = pag->getPTASVFStmtSet(SVFStmt::Phi);
    for (auto it = phis.begin(), eit = phis.end(); it != eit; ++it)
    {
        const PhiStmt* phi = SVFUtil::cast<PhiStmt>(*it);
        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
        {
            NodeID opId = phi->getOpVar(i)->getId();
            NodeID resId = phi->getResID();
            const ICFGNode* predNode = phi->getOpICFGNode(i);
            const PathCond* g = getBBGuard(predNode ? predNode->getBB() : nullptr);
            setCopyGuard(opId, resId, g);
        }
    }

    // --- Plain CopyStmt: guard from enclosing BB ---
    SVFStmt::SVFStmtSetTy& copies = pag->getPTASVFStmtSet(SVFStmt::Copy);
    for (auto it = copies.begin(), eit = copies.end(); it != eit; ++it)
    {
        const CopyStmt* c = SVFUtil::cast<CopyStmt>(*it);
        const PathCond* g = getBBGuard(c->getICFGNode() ? c->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setCopyGuard(c->getRHSVarID(), c->getLHSVarID(), g);
    }

    // --- LoadStmt: guard from enclosing BB ---
    auto setLoadGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Load});
        if (it == edgeGuards.end())
            edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::Load}] = guard;
        else
            it->second = PathCond::getOr(it->second, guard);
    };
    SVFStmt::SVFStmtSetTy& loads = pag->getPTASVFStmtSet(SVFStmt::Load);
    for (auto it = loads.begin(), eit = loads.end(); it != eit; ++it)
    {
        const LoadStmt* l = SVFUtil::cast<LoadStmt>(*it);
        const PathCond* g = getBBGuard(l->getICFGNode() ? l->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setLoadGuard(l->getRHSVarID(), l->getLHSVarID(), g);
    }

    // --- StoreStmt: guard from enclosing BB ---
    auto setStoreGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Store});
        if (it == edgeGuards.end())
            edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::Store}] = guard;
        else
            it->second = PathCond::getOr(it->second, guard);
    };
    SVFStmt::SVFStmtSetTy& stores = pag->getPTASVFStmtSet(SVFStmt::Store);
    for (auto it = stores.begin(), eit = stores.end(); it != eit; ++it)
    {
        const StoreStmt* st = SVFUtil::cast<StoreStmt>(*it);
        const PathCond* g = getBBGuard(st->getICFGNode() ? st->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setStoreGuard(st->getRHSVarID(), st->getLHSVarID(), g);
    }

    // --- GepStmt: guard from enclosing BB ---
    auto setGepGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Gep});
        if (it == edgeGuards.end())
            edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::Gep}] = guard;
        else
            it->second = PathCond::getOr(it->second, guard);
    };
    SVFStmt::SVFStmtSetTy& geps = pag->getPTASVFStmtSet(SVFStmt::Gep);
    for (auto it = geps.begin(), eit = geps.end(); it != eit; ++it)
    {
        const GepStmt* gs = SVFUtil::cast<GepStmt>(*it);
        const PathCond* g = getBBGuard(gs->getICFGNode() ? gs->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
            setGepGuard(gs->getRHSVarID(), gs->getLHSVarID(), g);
    }

    // --- SelectStmt: per-operand guard from enclosing BB ---
    SVFStmt::SVFStmtSetTy& selects = pag->getPTASVFStmtSet(SVFStmt::Select);
    for (auto it = selects.begin(), eit = selects.end(); it != eit; ++it)
    {
        const SelectStmt* sel = SVFUtil::cast<SelectStmt>(*it);
        const SVFVar* cond = sel->getCondition();
        if (!cond) continue;
        const PathCond* condAtom = PathCond::getAtom(cond->getId(), true);
        const PathCond* negCondAtom = PathCond::getAtom(cond->getId(), false);

        // true branch -> opVar(0)
        if (sel->getOpVarNum() > 0)
        {
            const PathCond* trueG = getBBGuard(sel->getICFGNode() ? sel->getICFGNode()->getBB() : nullptr);
            if (!trueG->isTrue())
                trueG = PathCond::getAnd(trueG, condAtom);
            else
                trueG = condAtom;
            setCopyGuard(sel->getOpVar(0)->getId(), sel->getResID(), trueG);
        }
        // false branch -> opVar(1)
        if (sel->getOpVarNum() > 1)
        {
            const PathCond* falseG = getBBGuard(sel->getICFGNode() ? sel->getICFGNode()->getBB() : nullptr);
            if (!falseG->isTrue())
                falseG = PathCond::getAnd(falseG, negCondAtom);
            else
                falseG = negCondAtom;
            setCopyGuard(sel->getOpVar(1)->getId(), sel->getResID(), falseG);
        }
    }

    // --- CallPE: per-operand guard from callsite BB ---
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

    // --- RetPE: guard from function exit BB ---
    SVFStmt::SVFStmtSetTy& retpes = pag->getPTASVFStmtSet(SVFStmt::Ret);
    for (auto it = retpes.begin(), eit = retpes.end(); it != eit; ++it)
    {
        const RetPE* ret = SVFUtil::cast<RetPE>(*it);
        const PathCond* g = getBBGuard(ret->getFunExitICFGNode() ? ret->getFunExitICFGNode()->getBB() : nullptr);
        setCopyGuard(ret->getRHSVarID(), ret->getLHSVarID(), g);
    }

    // --- AddrStmt: guard from enclosing BB ---
    SVFStmt::SVFStmtSetTy& addrs = pag->getPTASVFStmtSet(SVFStmt::Addr);
    for (auto it = addrs.begin(), eit = addrs.end(); it != eit; ++it)
    {
        const AddrStmt* addr = SVFUtil::cast<AddrStmt>(*it);
        const PathCond* g = getBBGuard(addr->getICFGNode() ? addr->getICFGNode()->getBB() : nullptr);
        if (!g->isTrue())
        {
            // Addr edges are handled specially; store guard for use in processAddr
            NodeID src = addr->getRHSVarID();
            NodeID dst = addr->getLHSVarID();
            auto itg = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic});
            if (itg == edgeGuards.end())
                edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic}] = g;
            else
                itg->second = PathCond::getOr(itg->second, g);
        }
    }
}

/*!
 * Look up the guard for a copy edge.
 * Priority: static guards > derived guards > True.
 */
const PathCond* ConditionalAndersen::getEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic});
    if (it != edgeGuards.end())
        return it->second;

    auto it2 = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyDerived});
    if (it2 != edgeGuards.end())
        return it2->second;

    return PathCond::getTrue();
}

const PathCond* ConditionalAndersen::getLoadEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Load});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

const PathCond* ConditionalAndersen::getStoreEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Store});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

const PathCond* ConditionalAndersen::getGepEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Gep});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

/*!
 * Apply k-limiting: if depth > k, collapse to True.
 */
const PathCond* ConditionalAndersen::applyKLimit(const PathCond* cond) const
{
    if (kLimit == 0) return cond;          // 0 = unlimited (no truncation)
    if (cond->depth() <= kLimit) return cond;
    return PathCond::getTrue();
}

/*!
 * Convert PathCond to Z3Expr for SAT checking.
 */
Z3Expr ConditionalAndersen::pathCondToZ3(const PathCond* cond) const
{
    if (cond->isTrue())  return Z3Expr::getTrueCond();
    if (cond->isFalse()) return Z3Expr::getFalseCond();
    if (cond->isAtom())
    {
        std::string name = "c" + std::to_string(cond->getBranchId());
        z3::expr var = Z3Expr::getContext().bool_const(name.c_str());
        if (!cond->isTrueBranch())
            var = !var;
        return Z3Expr(var);
    }
    if (cond->isAnd())
        return Z3Expr::AND(pathCondToZ3(cond->getLeft()),
                           pathCondToZ3(cond->getRight()));
    // isOr()
    return Z3Expr::OR(pathCondToZ3(cond->getLeft()),
                      pathCondToZ3(cond->getRight()));
}

/*!
 * Z3-based SAT check (with proper push/pop).
 */
bool ConditionalAndersen::z3IsSat(const PathCond* cond) const
{
    numZ3SatChecks++;
    if (cond->isTrue())  return true;
    if (cond->isFalse()) return false;

    if (useFastGuard)
    {
        FastGuard fg = FastGuard::fromPathCond(cond);
        return fg.isSat();
    }

    Z3Expr z3cond = pathCondToZ3(cond);
    z3::solver& s = Z3Expr::getSolver();
    s.push();
    s.add(z3cond.getExpr());
    z3::check_result r = s.check();
    s.pop();
    return r != z3::unsat;
}

/*!
 * OR-merge a guard onto an existing (var,obj) entry in condPtsMap.
 * Returns true iff the entry was newly inserted or the guard changed.
 */
bool ConditionalAndersen::orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard)
{
    auto& map = condPtsMap[var];
    auto it = map.find(obj);
    if (it != map.end())
    {
        const PathCond* merged = PathCond::getOr(it->second, guard);
        if (merged == it->second)
            return false; // no change
        it->second = merged;
        return true;
    }
    map[obj] = guard;
    return true;
}

/*!
 * Override SCC merge: synchronize conditional points-to and edge guards.
 */
bool ConditionalAndersen::mergeSrcToTgt(NodeID nodeId, NodeID newRepId)
{
    if (nodeId == newRepId)
        return false;

    // 1. Save edge guards involving nodeId
    std::unordered_map<EdgeGuardKey, const PathCond*, EdgeGuardKeyHash> guardsToMove;
    for (auto it = edgeGuards.begin(); it != edgeGuards.end(); )
    {
        if (it->first.src == nodeId || it->first.dst == nodeId)
        {
            guardsToMove[it->first] = it->second;
            it = edgeGuards.erase(it);
        }
        else
            ++it;
    }

    // 2. Merge condPtsMap: move all conditional pts from sub to rep (OR-merge)
    auto itPts = condPtsMap.find(nodeId);
    if (itPts != condPtsMap.end())
    {
        for (const auto& pair : itPts->second)
            orMergeCondPts(newRepId, pair.first, pair.second);
        condPtsMap.erase(itPts);
    }

    // 3. Delegate to parent for actual edge moving / pts merging / node removal
    bool pwc = Andersen::mergeSrcToTgt(nodeId, newRepId);

    // 4. Restore edge guards with updated keys (nodeId -> newRepId)
    for (const auto& pair : guardsToMove)
    {
        NodeID src = pair.first.src;
        NodeID dst = pair.first.dst;
        CondEdgeKind kind = pair.first.kind;
        if (src == nodeId) src = newRepId;
        if (dst == nodeId) dst = newRepId;
        EdgeGuardKey newKey = EdgeGuardKey{src, dst, kind};
        auto it = edgeGuards.find(newKey);
        if (it == edgeGuards.end())
            edgeGuards[newKey] = pair.second;
        else
            it->second = PathCond::getOr(it->second, pair.second);
    }

    return pwc;
}

/*!
 * Override inter-procedural edge connection to attach callsite guards.
 */
void ConditionalAndersen::connectCaller2CalleeParams(const CallICFGNode* cs,
        const FunObjVar* F, NodePairSet& cpySrcNodes)
{
    const PathCond* csGuard = getBBGuard(cs->getBB());

    NodePairSet newEdges;
    AndersenBase::connectCaller2CalleeParams(cs, F, newEdges);

    for (const auto& pair : newEdges)
    {
        auto key = EdgeGuardKey{pair.first, pair.second, CondEdgeKind::CopyDerived};
        auto it = edgeGuards.find(key);
        if (it == edgeGuards.end())
            edgeGuards[key] = csGuard;
        else
            it->second = PathCond::getOr(it->second, csGuard);
    }

    cpySrcNodes.insert(newEdges.begin(), newEdges.end());
}

/*!
 * Override inter-procedural fork edge connection to attach callsite guards.
 */
void ConditionalAndersen::connectCaller2ForkedFunParams(const CallICFGNode* cs,
        const FunObjVar* F, NodePairSet& cpySrcNodes)
{
    const PathCond* csGuard = getBBGuard(cs->getBB());

    NodePairSet newEdges;
    AndersenBase::connectCaller2ForkedFunParams(cs, F, newEdges);

    for (const auto& pair : newEdges)
    {
        auto key = EdgeGuardKey{pair.first, pair.second, CondEdgeKind::CopyDerived};
        auto it = edgeGuards.find(key);
        if (it == edgeGuards.end())
            edgeGuards[key] = csGuard;
        else
            it->second = PathCond::getOr(it->second, csGuard);
    }

    cpySrcNodes.insert(newEdges.begin(), newEdges.end());
}

/*!
 * Process address edge: add unconditional (True, obj) to condPtsMap.
 */
void ConditionalAndersen::processAddr(const AddrCGEdge* addr)
{
    Andersen::processAddr(addr);

    NodeID dst = addr->getDstID();
    NodeID src = addr->getSrcID();

    if (orMergeCondPts(dst, src, PathCond::getTrue()))
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
            NodeID obj = pair.first;
            const PathCond* cond = pair.second;
            const PathCond* newCond = applyKLimit(PathCond::getAnd(cond, guard));

            if (eagerSat && !z3IsSat(newCond)) continue;

            if (orMergeCondPts(dst, obj, newCond))
                condChanged = true;
        }
    }

    if (condChanged)
        pushIntoWorklist(dst);

    return parentChanged || condChanged;
}

/*!
 * Process load edge: create derived copy edge with guard.
 * Guard = load_edge_guard ∧ OR(conditions under which pointer points to node).
 */
bool ConditionalAndersen::processLoad(NodeID node, const ConstraintEdge* load)
{
    bool parentChanged = Andersen::processLoad(node, load);

    NodeID pointer = load->getSrcID();
    NodeID dst = load->getDstID();

    const PathCond* ptsG = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        const PathCond* acc = PathCond::getFalse();
        bool found = false;
        for (const auto& pair : it->second)
        {
            if (pair.first == node)
            {
                acc = PathCond::getOr(acc, pair.second);
                found = true;
            }
        }
        if (found)
            ptsG = acc;
    }

    const PathCond* loadG = getLoadEdgeGuard(pointer, dst);
    const PathCond* guard;
    if (loadG->isTrue()) guard = ptsG;
    else if (ptsG->isTrue()) guard = loadG;
    else guard = PathCond::getAnd(loadG, ptsG);

    auto itg = edgeGuards.find(EdgeGuardKey{node, dst, CondEdgeKind::CopyDerived});
    if (itg == edgeGuards.end())
        edgeGuards[EdgeGuardKey{node, dst, CondEdgeKind::CopyDerived}] = guard;
    else
        itg->second = PathCond::getOr(itg->second, guard);

    return parentChanged;
}

/*!
 * Process store edge: create derived copy edge with guard.
 * Guard = store_edge_guard ∧ OR(conditions under which pointer points to node).
 */
bool ConditionalAndersen::processStore(NodeID node, const ConstraintEdge* store)
{
    bool parentChanged = Andersen::processStore(node, store);

    NodeID src = store->getSrcID();
    NodeID pointer = store->getDstID();

    const PathCond* ptsG = PathCond::getTrue();
    auto it = condPtsMap.find(pointer);
    if (it != condPtsMap.end())
    {
        const PathCond* acc = PathCond::getFalse();
        bool found = false;
        for (const auto& pair : it->second)
        {
            if (pair.first == node)
            {
                acc = PathCond::getOr(acc, pair.second);
                found = true;
            }
        }
        if (found)
            ptsG = acc;
    }

    const PathCond* storeG = getStoreEdgeGuard(src, pointer);
    const PathCond* guard;
    if (storeG->isTrue()) guard = ptsG;
    else if (ptsG->isTrue()) guard = storeG;
    else guard = PathCond::getAnd(storeG, ptsG);

    auto itg = edgeGuards.find(EdgeGuardKey{src, node, CondEdgeKind::CopyDerived});
    if (itg == edgeGuards.end())
        edgeGuards[EdgeGuardKey{src, node, CondEdgeKind::CopyDerived}] = guard;
    else
        itg->second = PathCond::getOr(itg->second, guard);

    return parentChanged;
}

/*!
 * Process gep edge: propagate conditional points-to with field translation.
 */
bool ConditionalAndersen::processGep(NodeID, const GepCGEdge* edge)
{
    bool parentChanged = Andersen::processGep(edge->getSrcID(), edge);

    NodeID src = edge->getSrcID();
    NodeID dst = edge->getDstID();
    const PathCond* edgeG = getGepEdgeGuard(src, dst);
    const bool edgeIsTrue = edgeG->isTrue();

    bool condChanged = false;
    bool isVariant = SVFUtil::isa<VariantGepCGEdge>(edge);
    const NormalGepCGEdge* normalGep =
        SVFUtil::dyn_cast<NormalGepCGEdge>(edge);
    assert((isVariant || normalGep) && "unknown gep edge kind");

    auto it = condPtsMap.find(src);
    if (it != condPtsMap.end())
    {
        for (const auto& pair : it->second)
        {
            NodeID o = pair.first;
            const PathCond* og = pair.second;
            if (og->isFalse()) continue;

            const PathCond* g = edgeIsTrue ? og : PathCond::getAnd(og, edgeG);
            if (eagerSat && !z3IsSat(g)) continue;

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

            if (orMergeCondPts(dst, newField, g))
                condChanged = true;
        }
    }

    if (condChanged) pushIntoWorklist(dst);
    return parentChanged || condChanged;
}

/*!
 * Alias query using conditional points-to.
 * May-alias only if there exists a common object under a satisfiable conjunction.
 */
AliasResult ConditionalAndersen::alias(NodeID v1, NodeID v2)
{
    numAliasTotal++;
    if (v1 == v2) return MustAlias;

    CondPointsTo pts1 = expandCondFIObjs(getCondPts(v1));
    CondPointsTo pts2 = expandCondFIObjs(getCondPts(v2));

    if (pts1.empty() || pts2.empty())
        return NoAlias;

    for (const auto& p1 : pts1)
    {
        auto it2 = pts2.find(p1.first);
        if (it2 == pts2.end()) continue;
        const PathCond* conj = PathCond::getAnd(p1.second, it2->second);
        if (z3IsSat(conj))
        {
            numAliasRefined++;
            return AliasResult::MayAlias;
        }
    }

    return AliasResult::NoAlias;
}

AliasResult ConditionalAndersen::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1->getId() == v2->getId()) return MustAlias;
    return alias(v1->getId(), v2->getId());
}

/*!
 * Expand field-insensitive objects in a conditional points-to set.
 * If an object is a base object or field-insensitive, include all its fields.
 */
ConditionalAndersen::CondPointsTo ConditionalAndersen::expandCondFIObjs(const CondPointsTo& pts) const
{
    CondPointsTo expanded;
    for (const auto& pair : pts)
    {
        NodeID obj = pair.first;
        const PathCond* guard = pair.second;
        expanded[obj] = guard;

        NodeID baseObj = pag->getBaseObjVarID(obj);
        if (baseObj == obj || isFieldInsensitive(obj))
        {
            const NodeBS& fields = pag->getAllFieldsObjVars(obj);
            for (const NodeID f : fields)
            {
                auto it = expanded.find(f);
                if (it == expanded.end())
                    expanded[f] = guard;
                else
                    it->second = PathCond::getOr(it->second, guard);
            }
        }
    }
    return expanded;
}

/*!
 * Dump edge guards for debugging.
 */
void ConditionalAndersen::dumpEdgeGuards() const
{
    auto dumpByKind = [&](CondEdgeKind kind, const char* label)
    {
        SVFUtil::outs() << "--- " << label << " ---\n";
        for (const auto& pair : edgeGuards)
        {
            if (pair.first.kind == kind)
            {
                SVFUtil::outs() << "  (" << pair.first.src << " -> "
                                << pair.first.dst << "): "
                                << pair.second->toString() << "\n";
            }
        }
    };

    SVFUtil::outs() << "\n========== Conditional Andersen Edge Guards ==========\n";
    dumpByKind(CondEdgeKind::CopyStatic, "Static Copy Guards");
    dumpByKind(CondEdgeKind::CopyDerived, "Derived Copy Guards");
    dumpByKind(CondEdgeKind::Load, "Load Guards");
    dumpByKind(CondEdgeKind::Store, "Store Guards");
    dumpByKind(CondEdgeKind::Gep, "GEP Guards");
    SVFUtil::outs() << "=====================================================\n\n";
}

/*!
 * Finalize: optionally print conditional points-to for debugging (Phase 1).
 */
void ConditionalAndersen::finalize()
{
    Andersen::finalize();

    // Count conditional points-to entries
    numCondPtsEntries = 0;
    for (const auto& entry : condPtsMap)
        numCondPtsEntries += entry.second.size();

    // Print statistics
    SVFUtil::outs() << "\n========== Conditional Andersen Statistics ==========\n";
    SVFUtil::outs() << "  kLimit:              " << kLimit << "\n";
    SVFUtil::outs() << "  eagerSat:            " << (eagerSat ? "true" : "false") << "\n";
    SVFUtil::outs() << "  PWC enabled:         " << (Options::CondAnderPWC() ? "true" : "false") << "\n";
    SVFUtil::outs() << "  Z3 SAT checks:       " << numZ3SatChecks << "\n";
    SVFUtil::outs() << "  Alias queries:       " << numAliasTotal << "\n";
    SVFUtil::outs() << "  Alias refined (May): " << numAliasRefined << "\n";
    SVFUtil::outs() << "  CondPts entries:     " << numCondPtsEntries << "\n";
    auto countByKind = [&](CondEdgeKind kind) -> size_t
    {
        size_t c = 0;
        for (const auto& pair : edgeGuards)
            if (pair.first.kind == kind) ++c;
        return c;
    };
    SVFUtil::outs() << "  Static copy guards:  " << countByKind(CondEdgeKind::CopyStatic) << "\n";
    SVFUtil::outs() << "  Derived copy guards: " << countByKind(CondEdgeKind::CopyDerived) << "\n";
    SVFUtil::outs() << "  Load guards:         " << countByKind(CondEdgeKind::Load) << "\n";
    SVFUtil::outs() << "  Store guards:        " << countByKind(CondEdgeKind::Store) << "\n";
    SVFUtil::outs() << "  GEP guards:          " << countByKind(CondEdgeKind::Gep) << "\n";
    SVFUtil::outs() << "=====================================================\n\n";

    if (Options::CondAnderDumpGuards())
        dumpEdgeGuards();

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
                NodeID obj = pair.first;
                const SVFVar* objVar = pag->getGNode(obj);
                SVFUtil::outs() << "(" << pair.second->toString() << ", "
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
