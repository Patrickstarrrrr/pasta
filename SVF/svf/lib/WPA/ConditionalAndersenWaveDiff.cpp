//===- ConditionalAndersen.cpp -- Path-aware Andersen analysis--------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#include "WPA/ConditionalAndersenWaveDiff.h"
#include "SVFIR/SVFStatements.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include <execinfo.h>
#include <stdlib.h>

using namespace SVF;
using namespace SVFUtil;

ConditionalAndersenWaveDiff::ConditionalAndersenWaveDiff(SVFIR* _pag, PTATY type)
    : AndersenWaveDiff(_pag, type),
      kLimit(Options::CondAnderKLimit()),
      eagerSat(Options::CondAnderEagerSat()),
      useFastGuard(Options::CondAnderFastGuard()),
      useDepthLimit(Options::CondAnderUseDepthLimit()),
      mLimit(Options::CondAnderMLimit()),
      nLimit(Options::CondAnderNLimit()),
      mergeCondSCC(Options::CondAnderMergeCondSCC()),
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
void ConditionalAndersenWaveDiff::initialize()
{
    AndersenWaveDiff::initialize();
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
const PathCond* ConditionalAndersenWaveDiff::getBBGuard(const SVFBasicBlock* bb) const
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

void ConditionalAndersenWaveDiff::attachStaticEdgeGuards()
{
    auto setCopyGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic});
        if (it == edgeGuards.end())
            edgeGuards[EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic}] = guard;
        else
            it->second = PathCond::getOr(it->second, guard);
    };

    // --- PhiStmt: guard from phi's enclosing BB ---
    // NOTE: We use the phi BB guard (not per-operand incoming-block guard)
    // for simplicity.  Experiments show that per-operand guard only improves
    // precision on 1/23 test cases (05_phi_assign, by 1 MayAlias) while
    // leaving all others identical.  The original per-operand implementation
    // is kept below in comments for reference.
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
    /*
    // --- Original: per-operand guard from operand's incoming edge ---
    // This is semantically more precise (each operand is selected only on
    // its incoming path), but in practice gives almost identical alias
    // results on LLVM SSA IR because phi-BB guard is usually True.
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
    */

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
const PathCond* ConditionalAndersenWaveDiff::getEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyStatic});
    if (it != edgeGuards.end())
        return it->second;

    auto it2 = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::CopyDerived});
    if (it2 != edgeGuards.end())
        return it2->second;

    return PathCond::getTrue();
}

const PathCond* ConditionalAndersenWaveDiff::getLoadEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Load});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

const PathCond* ConditionalAndersenWaveDiff::getStoreEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Store});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

const PathCond* ConditionalAndersenWaveDiff::getGepEdgeGuard(NodeID src, NodeID dst) const
{
    auto it = edgeGuards.find(EdgeGuardKey{src, dst, CondEdgeKind::Gep});
    if (it != edgeGuards.end())
        return it->second;
    return PathCond::getTrue();
}

/*!
 * Extract leaves of a pure And chain in in-order traversal.
 */
std::vector<const PathCond*> ConditionalAndersenWaveDiff::extractAndChain(const PathCond* cond) const
{
    std::vector<const PathCond*> leaves;
    if (cond->isPureAndChain())
        cond->extractAndLeaves(leaves);
    return leaves;
}

/*!
 * Build a left-associative And chain from a list of literals.
 */
const PathCond* ConditionalAndersenWaveDiff::buildAndChain(const std::vector<const PathCond*>& literals) const
{
    if (literals.empty()) return PathCond::getTrue();
    const PathCond* result = literals[0];
    for (size_t i = 1; i < literals.size(); ++i)
        result = PathCond::getAnd(result, literals[i]);
    return result;
}

/*!
 * Count DNF clauses using FastGuard conversion.
 */
u32_t ConditionalAndersenWaveDiff::countClauses(const PathCond* cond) const
{
    if (cond->isCapped()) return nLimit + 1; // capped guard is treated as exceeding the limit
    if (cond->isTrue() || cond->isFalse() || cond->isAtom()) return 1;
    // Use cached clauseCount from PathCond AST instead of expensive FastGuard DNF conversion.
    // clauseCount is an upper bound, which is safe for limit checking:
    // if upper bound > nLimit, the actual count is definitely > nLimit.
    return cond->clauseCount(nLimit + 1);
}

/*!
 * Apply guard limits.
 *
 * Two modes:
 *   1. Depth-based (useDepthLimit=true): if AST depth > kLimit, collapse to True.
 *   2. M/N-based (useDepthLimit=false):
 *      - Pure And chains exceeding mLimit are truncated to the most recent m literals.
 *        The truncated guard is marked as conj-capped in conjCappedGuards.
 *      - Guards with more than nLimit DNF clauses are collapsed to True (disj-capped).
 */
const PathCond* ConditionalAndersenWaveDiff::applyLimits(const PathCond* cond) const
{
    // kLimit == 0 means truncate all guards to True (unconditional mode).
    if (kLimit == 0) return PathCond::getTrue();

    // Mode 1: depth-based k-limit (legacy)
    if (useDepthLimit)
    {
        if (kLimit == -1) return cond;
        if (cond->depth() <= static_cast<u32_t>(kLimit)) return cond;
        return PathCond::getTrue();
    }

    // Mode 2: m/n-based limits (default)
    const PathCond* result = cond;

    // Step A: m-limit (conjunctive truncation)
    if (mLimit > 0 && result->isPureAndChain())
    {
        std::vector<const PathCond*> leaves;
        result->extractAndLeaves(leaves);
        // Deduplicate while preserving "most recent" order:
        // scan from right to left, keep first occurrence of each unique literal.
        // Use (branchId, trueBranch) as key since PathCond atoms may be different
        // objects with the same logical meaning.
        std::vector<const PathCond*> dedup;
        Set<std::pair<NodeID, bool>> seen;
        for (auto it = leaves.rbegin(); it != leaves.rend(); ++it)
        {
            auto key = std::make_pair((*it)->getBranchId(), (*it)->isTrueBranch());
            if (seen.insert(key).second)
                dedup.push_back(*it);
        }
        std::reverse(dedup.begin(), dedup.end());

        if (dedup.size() > mLimit)
        {
            // Truncate: keep the most recent m unique literals
            std::vector<const PathCond*> trimmed(dedup.end() - mLimit, dedup.end());
            result = buildAndChain(trimmed);
            conjCappedGuards.insert(result);
        }
    }

    // Step B: n-limit (disjunctive collapse)
    if (nLimit > 0)
    {
        u32_t clauses = countClauses(result);
        if (clauses > nLimit)
        {
            result = PathCond::getCappedTrue();
        }
    }

    return result;
}

/*!
 * Convert PathCond to Z3Expr for SAT checking.
 */
Z3Expr ConditionalAndersenWaveDiff::pathCondToZ3(const PathCond* cond) const
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
bool ConditionalAndersenWaveDiff::z3IsSat(const PathCond* cond) const
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
bool ConditionalAndersenWaveDiff::orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard)
{
    (void)var; (void)obj; // silence unused warnings if no debug
    auto& map = condPtsMap[var];
    auto it = map.find(obj);
    if (it != map.end())
    {
        const PathCond* merged = PathCond::getOr(it->second, guard);
        /* debug removed */
        if (merged == it->second)
            return false; // no change
        // Apply limits to prevent Or-tree from growing unbounded.
        // For m/n-limit mode, large guards will be capped here before they
        // cause exponential blow-up in future operations.
        if (!useDepthLimit)
            merged = applyLimits(merged);
        it->second = merged;
        return true;
    }
    map[obj] = guard;
    return true;
}

/*!
 * Override SCC merge: synchronize conditional points-to and edge guards.
 */
bool ConditionalAndersenWaveDiff::mergeSrcToTgt(NodeID nodeId, NodeID newRepId)
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

    // 2. Merge condPtsMap: move all conditional pts from sub to rep.
    //    In merge-cond-SCC mode, over-approximate guards to True.
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
 * Override processNode to set up conditional diff propagation for Phase 1.
 */
void ConditionalAndersenWaveDiff::processNode(NodeID nodeId)
{
    if (!consCG->hasGNode(nodeId))
        return;
    currentDiffNode = nodeId;
    currentDiffObjs.clear();
    auto diffIt = condDiffPtsMap.find(nodeId);
    if (diffIt != condDiffPtsMap.end())
    {
        currentDiffObjs.clear();
        for (NodeID obj : diffIt->second)
            currentDiffObjs.push_back(obj);
        condDiffPtsMap.erase(diffIt);
    }
    AndersenWaveDiff::processNode(nodeId);

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
                handleCopyGep(subNode);
            }
        }
    }
}

/*!
 * Override postProcessNode to set up conditional diff propagation for Phase 2.
 *
 * Unlike standard AndersenWaveDiff, conditional SCCs may be preserved (not merged).
 * For unmerged SCCs, copy/gep propagation within the cycle cannot be fully handled
 * in a single topo-order pass, so we also invoke handleCopyGep here to ensure
 * the SCC reaches a fixpoint.
 */
void ConditionalAndersenWaveDiff::postProcessNode(NodeID nodeId)
{
    if (!consCG->hasGNode(nodeId))
        return;
    currentDiffNode = nodeId;
    currentDiffObjs.clear();
    auto diffIt = condDiffPtsMap.find(nodeId);
    if (diffIt != condDiffPtsMap.end())
    {
        currentDiffObjs.clear();
        for (NodeID obj : diffIt->second)
            currentDiffObjs.push_back(obj);
        condDiffPtsMap.erase(diffIt);
    }
    AndersenWaveDiff::postProcessNode(nodeId);

    // For preserved SCCs, copy/gep edges may need additional propagation
    // beyond Phase 1 topo-order.  handleCopyGep uses diffPts, so already-
    // converged nodes incur negligible overhead.
    // Also process sub-nodes because their edges were not moved to the rep.
    if (sccRepNode(nodeId) == nodeId)
    {
        ConstraintNode* node = consCG->getConstraintNode(nodeId);
        handleCopyGep(node);

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
                    // handle load/store for sub-node (may add new edges)
                    for (ConstraintEdge* edge : subNode->getOutEdges())
                    {
                        if (edge->getEdgeKind() == ConstraintEdge::Load)
                        {
                            if (handleLoad(subId, edge))
                                reanalyze = true;
                        }
                    }
                    for (ConstraintEdge* edge : subNode->getInEdges())
                    {
                        if (edge->getEdgeKind() == ConstraintEdge::Store)
                        {
                            if (handleStore(subId, edge))
                                reanalyze = true;
                        }
                    }
                    // handle copy/gep for sub-node
                    handleCopyGep(subNode);
                }
            }
        }
    }
}

/*!
 * Override handleCopyGep for wave-diff to process all outgoing copy/gep edges
 * whenever there is either a base diff or conditional points-to entries.
 *
 * We use hasCondPts (check condPtsMap) instead of hasCondDiff because
 * condDiffPtsMap is not populated in this implementation; we rely on the
 * standard diff-pts mechanism for unconditional propagation and propagate
 * conditional entries whenever the node has any.
 */
void ConditionalAndersenWaveDiff::handleCopyGep(ConstraintNode* node)
{
    NodeID nodeId = node->getId();
    computeDiffPts(nodeId);
    bool hasBaseDiff = !getDiffPts(nodeId).empty();
    bool hasCondPts = condPtsMap.find(nodeId) != condPtsMap.end();
    if (hasBaseDiff || hasCondPts)
    {
        for (ConstraintEdge* edge : node->getCopyOutEdges())
            processCopy(nodeId, edge);
        for (ConstraintEdge* edge : node->getGepOutEdges())
        {
            if (GepCGEdge* gepEdge = SVFUtil::dyn_cast<GepCGEdge>(edge))
                processGep(nodeId, gepEdge);
        }
    }
}

/*!
 * Check whether an SCC (represented by its rep node) contains any
 * constraint edge with a non-trivial conditional guard.
 */
bool ConditionalAndersenWaveDiff::sccHasConditionalEdge(NodeID repId) const
{
    const NodeBS& subNodes = getSCCDetector()->subNodes(repId);
    for (NodeBS::iterator it = subNodes.begin(), eit = subNodes.end(); it != eit; ++it)
    {
        NodeID nodeId = *it;
        ConstraintNode* node = consCG->getConstraintNode(nodeId);
        // Outgoing edges
        for (ConstraintEdge* edge : node->getOutEdges())
        {
            if (edge->getEdgeKind() == ConstraintEdge::Copy)
            {
                const PathCond* g = getEdgeGuard(nodeId, edge->getDstID());
                if (!g->isTrue()) return true;
            }
            else if (edge->getEdgeKind() == ConstraintEdge::NormalGep ||
                     edge->getEdgeKind() == ConstraintEdge::VariantGep)
            {
                const PathCond* g = getGepEdgeGuard(nodeId, edge->getDstID());
                if (!g->isTrue()) return true;
            }
        }
        // Incoming edges (also part of the SCC cycle)
        for (ConstraintEdge* edge : node->getInEdges())
        {
            if (edge->getEdgeKind() == ConstraintEdge::Copy)
            {
                const PathCond* g = getEdgeGuard(edge->getSrcID(), nodeId);
                if (!g->isTrue()) return true;
            }
            else if (edge->getEdgeKind() == ConstraintEdge::NormalGep ||
                     edge->getEdgeKind() == ConstraintEdge::VariantGep)
            {
                const PathCond* g = getGepEdgeGuard(edge->getSrcID(), nodeId);
                if (!g->isTrue()) return true;
            }
        }
    }
    return false;
}

/*!
 * Override SCC detection: optionally skip merging SCCs that contain
 * conditional edges, preserving per-node conditional precision.
 */
NodeStack& ConditionalAndersenWaveDiff::SCCDetect()
{
    numOfSCCDetection++;

    double sccStart = stat->getClk();
    WPAConstraintSolver::SCCDetect();
    double sccEnd = stat->getClk();
    timeOfSCCDetection += (sccEnd - sccStart) / TIMEINTERVAL;

    double mergeStart = stat->getClk();

    NodeStack topoOrder = getSCCDetector()->topoNodeStack();
    while (!topoOrder.empty())
    {
        NodeID repNodeId = topoOrder.top();
        topoOrder.pop();
        const NodeBS& subNodes = getSCCDetector()->subNodes(repNodeId);

        // If the SCC has only one node, nothing to merge.
        if (subNodes.count() <= 1)
            continue;

        // Check if this SCC contains conditional edges.
        bool hasCond = sccHasConditionalEdge(repNodeId);

        if (hasCond && !mergeCondSCC)
        {
            // Skip merge: preserve per-node conditional points-to sets.
            // The worklist will continue propagating within the cycle,
            // but And-subset absorption in getOr keeps guards bounded.
            continue;
        }

        // Otherwise: merge all sub nodes to rep node (standard Andersen behavior)
        for (NodeBS::iterator nodeIt = subNodes.begin(); nodeIt != subNodes.end(); ++nodeIt)
        {
            NodeID subNodeId = *nodeIt;
            if (subNodeId != repNodeId)
                mergeNodeToRep(subNodeId, repNodeId);
        }
    }

    double mergeEnd = stat->getClk();
    timeOfSCCMerges += (mergeEnd - mergeStart) / TIMEINTERVAL;

    return getSCCDetector()->topoNodeStack();
}
void ConditionalAndersenWaveDiff::solveWorklist()
{
    // SCC detection: merge unconditional SCCs, skip conditional SCCs.
    NodeStack& nodeStack = SCCDetect();

    // Process nodes in reverse topological order.
    while (!nodeStack.empty())
    {
        NodeID nodeId = nodeStack.top();
        nodeStack.pop();
        collapsePWCNode(nodeId);
        processNode(nodeId);
        collapseFields();
    }

    // Continue with standard worklist processing.
    while (!isWorklistEmpty())
    {
        NodeID nodeId = popFromWorklist();
        postProcessNode(nodeId);
        collapseFields();
    }
}

/*!
 * Override inter-procedural edge connection to attach callsite guards.
 */
void ConditionalAndersenWaveDiff::connectCaller2CalleeParams(const CallICFGNode* cs,
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
void ConditionalAndersenWaveDiff::connectCaller2ForkedFunParams(const CallICFGNode* cs,
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
void ConditionalAndersenWaveDiff::processAddr(const AddrCGEdge* addr)
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
bool ConditionalAndersenWaveDiff::processCopy(NodeID node, const ConstraintEdge* edge)
{
    bool parentChanged = Andersen::processCopy(node, edge);

    NodeID dst = edge->getDstID();
    const PathCond* guard = getEdgeGuard(node, dst);
    /* debug removed */

    bool condChanged = false;
    auto it = condPtsMap.find(node);
    if (it != condPtsMap.end())
    {
        for (const auto& pair : it->second)
        {
            NodeID obj = pair.first;
            const PathCond* cond = pair.second;

            // If conj-capped, ignore further And operations
            const PathCond* newCond;
            if (!useDepthLimit && isConjCapped(cond))
            {
                newCond = cond;
            }
            else
            {
                newCond = applyLimits(PathCond::getAnd(cond, guard));
            }

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
bool ConditionalAndersenWaveDiff::processLoad(NodeID node, const ConstraintEdge* load)
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
bool ConditionalAndersenWaveDiff::processStore(NodeID node, const ConstraintEdge* store)
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
bool ConditionalAndersenWaveDiff::processGep(NodeID, const GepCGEdge* edge)
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
AliasResult ConditionalAndersenWaveDiff::alias(NodeID v1, NodeID v2)
{
    numAliasTotal++;
    if (v1 == v2) return MustAlias;

    // 1. Check unconditional points-to first.
    NodeID n1 = consCG->sccRepNode(v1);
    NodeID n2 = consCG->sccRepNode(v2);
    if (!BVDataPTAImpl::alias(getPts(n1), getPts(n2)))
        return AliasResult::NoAlias;

    // 2. Unconditional pts intersect; check conditional guards.
    CondPointsTo pts1 = expandCondFIObjs(getCondPts(v1));
    CondPointsTo pts2 = expandCondFIObjs(getCondPts(v2));

    /* debug removed */

    // If conditional map is incomplete, fall back conservatively.
    if (pts1.empty() || pts2.empty())
        return AliasResult::MayAlias;

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

AliasResult ConditionalAndersenWaveDiff::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1->getId() == v2->getId()) return MustAlias;
    return alias(v1->getId(), v2->getId());
}

/*!
 * Expand field-insensitive objects in a conditional points-to set.
 * If an object is a base object or field-insensitive, include all its fields.
 */
ConditionalAndersenWaveDiff::CondPointsTo ConditionalAndersenWaveDiff::expandCondFIObjs(const CondPointsTo& pts) const
{
    CondPointsTo expanded;
    for (const auto& pair : pts)
    {
        NodeID obj = pair.first;
        const PathCond* guard = pair.second;
        expanded[obj] = guard;

        // Skip objects that have been removed from PAG (e.g., by normalizePointsTo)
        if (!pag->hasGNode(obj))
            continue;

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
void ConditionalAndersenWaveDiff::dumpEdgeGuards() const
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
void ConditionalAndersenWaveDiff::finalize()
{
    Andersen::finalize();

    // Count conditional points-to entries
    numCondPtsEntries = 0;
    for (const auto& entry : condPtsMap)
        numCondPtsEntries += entry.second.size();

    // Print statistics
    SVFUtil::outs() << "\n========== Conditional Andersen Statistics ==========\n";
    if (useDepthLimit)
    {
        SVFUtil::outs() << "  Limit mode:          depth-based\n";
        SVFUtil::outs() << "  kLimit:              " << kLimit << "\n";
    }
    else
    {
        SVFUtil::outs() << "  Limit mode:          m/n-based\n";
        SVFUtil::outs() << "  mLimit (conj):       " << mLimit << "\n";
        SVFUtil::outs() << "  nLimit (disj):       " << nLimit << "\n";
        SVFUtil::outs() << "  Conj-capped guards:  " << conjCappedGuards.size() << "\n";
    }
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
const ConditionalAndersenWaveDiff::CondPointsTo& ConditionalAndersenWaveDiff::getCondPts(NodeID id) const
{
    NodeID rep = consCG->sccRepNode(id);
    auto it = condPtsMap.find(rep);
    if (it != condPtsMap.end())
        return it->second;

    static CondPointsTo empty;
    return empty;
}
