//===- ConditionalAndersen.cpp -- Path-aware Andersen analysis--------------//
//
//                     SVF: Static Value-Flow Analysis
//
//===----------------------------------------------------------------------===//

#include "WPA/ConditionalAndersen.h"
#include "SVFIR/SVFStatements.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include <random>

using namespace SVF;
using namespace SVFUtil;

/// Enable fine-grained conditional profiling via COND_PROFILE env var.
/// Default off because clock() is expensive on macOS (~290ns/call).
static bool condProfile = (getenv("COND_PROFILE") != nullptr);

ConditionalAndersen::ConditionalAndersen(SVFIR* _pag, PTATY type)
    : Andersen(_pag, type),
      kLimit(Options::CondAnderKLimit()),
      eagerSat(Options::CondAnderEagerSat()),
      useFastGuard(Options::CondAnderFastGuard()),
      useDepthLimit(Options::CondAnderUseDepthLimit()),
      mLimit(Options::CondAnderMLimit()),
      nLimit(Options::CondAnderNLimit()),
      mergeCondSCC(Options::CondAnderMergeCondSCC()),
      numZ3SatChecks(0),
      numAliasRefined(0),
      numAliasRefinedToNoAlias(0),
      numAliasTotal(0),
      numCondPtsEntries(0),
      timeCondProp(0.0),
      timeCondAlias(0.0),
      timeCondSCCMerge(0.0),
      timeGuardLimit(0.0),
      timeSATCheck(0.0)
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
        mergeCopyEdgeGuard(src, dst, guard, false); // do NOT create new edges in static attachment
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

    // --- StoreStmt: guard from enclosing BB ---
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

    // --- GepStmt: guard from enclosing BB ---
    auto setGepGuard = [&](NodeID src, NodeID dst, const PathCond* guard)
    {
        if (!guard || guard->isTrue()) return;
        ConstraintNode* srcNode = consCG->getConstraintNode(src);
        for (ConstraintEdge* e : srcNode->getOutEdges())
        {
            if (e->getDstID() == dst && (e->getEdgeKind() == ConstraintEdge::NormalGep || e->getEdgeKind() == ConstraintEdge::VariantGep))
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
    // Addr edge guards are not used by processAddr (True is implicit),
    // so we do not store them.
}

/*!
 * Look up the guard for a copy edge.
 * Priority: static guards > derived guards > True.
 */
const PathCond* ConditionalAndersen::getEdgeGuard(NodeID src, NodeID dst) const
{
    ConstraintNode* srcNode = consCG->getConstraintNode(src);
    if (!srcNode)
        return PathCond::getTrue();
    for (ConstraintEdge* edge : srcNode->getOutEdges())
    {
        if (edge->getEdgeKind() == ConstraintEdge::Copy && edge->getDstID() == dst)
        {
            const PathCond* g = edge->getGuard();
            return g ? g : PathCond::getTrue();
        }
    }
    return PathCond::getTrue();
}

void ConditionalAndersen::mergeCopyEdgeGuard(NodeID src, NodeID dst, const PathCond* guard, bool createIfMissing)
{
    if (!guard || guard->isTrue()) return;
    CopyCGEdge* edge = nullptr;
    if (createIfMissing)
    {
        edge = consCG->addCopyCGEdge(src, dst, guard);
    }
    if (!edge)
    {
        // Edge already exists (or we don't want to create); find it via directEdgeSet (O(log N)) and OR-merge the guard
        ConstraintNode* srcNode = consCG->getConstraintNode(src);
        ConstraintNode* dstNode = consCG->getConstraintNode(dst);
        ConstraintEdge keyEdge(srcNode, dstNode, ConstraintEdge::Copy);
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
 * Extract leaves of a pure And chain in in-order traversal.
 */
std::vector<const PathCond*> ConditionalAndersen::extractAndChain(const PathCond* cond) const
{
    std::vector<const PathCond*> leaves;
    if (cond->isPureAndChain())
        cond->extractAndLeaves(leaves);
    return leaves;
}

/*!
 * Build a left-associative And chain from a list of literals.
 */
const PathCond* ConditionalAndersen::buildAndChain(const std::vector<const PathCond*>& literals) const
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
u32_t ConditionalAndersen::countClauses(const PathCond* cond) const
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
const PathCond* ConditionalAndersen::applyLimits(const PathCond* cond) const
{
    double tStart = condProfile ? stat->getClk(true) : 0.0;
    // kLimit == 0 means truncate all guards to True (unconditional mode).
    if (kLimit == 0)
    {
        if (condProfile) timeGuardLimit += (stat->getClk(true) - tStart) / TIMEINTERVAL;
        return PathCond::getTrue();
    }

    // Mode 1: depth-based k-limit (legacy)
    if (useDepthLimit)
    {
        if (kLimit == -1)
        {
            if (condProfile) timeGuardLimit += (stat->getClk(true) - tStart) / TIMEINTERVAL;
            return cond;
        }
        if (cond->depth() <= static_cast<u32_t>(kLimit))
        {
            if (condProfile) timeGuardLimit += (stat->getClk(true) - tStart) / TIMEINTERVAL;
            return cond;
        }
        if (condProfile) timeGuardLimit += (stat->getClk(true) - tStart) / TIMEINTERVAL;
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

    if (condProfile) timeGuardLimit += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return result;
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
    double tStart = condProfile ? stat->getClk(true) : 0.0;
    numZ3SatChecks++;
    if (cond->isTrue())
    {
        if (condProfile) timeSATCheck += (stat->getClk(true) - tStart) / TIMEINTERVAL;
        return true;
    }
    if (cond->isFalse())
    {
        if (condProfile) timeSATCheck += (stat->getClk(true) - tStart) / TIMEINTERVAL;
        return false;
    }

    if (useFastGuard)
    {
        FastGuard fg = FastGuard::fromPathCond(cond);
        bool sat = fg.isSat();
        if (condProfile) timeSATCheck += (stat->getClk(true) - tStart) / TIMEINTERVAL;
        return sat;
    }

    Z3Expr z3cond = pathCondToZ3(cond);
    z3::solver& s = Z3Expr::getSolver();
    s.push();
    s.add(z3cond.getExpr());
    z3::check_result r = s.check();
    s.pop();
    if (condProfile) timeSATCheck += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return r != z3::unsat;
}

/*!
 * OR-merge a guard onto an existing (var,obj) entry in condPtsMap.
 * Returns true iff the entry was newly inserted or the guard changed.
 */
bool ConditionalAndersen::orMergeCondPts(NodeID var, NodeID obj, const PathCond* guard)
{
    // True guards are implicit: if the merged result is True, erase the entry.
    if (guard->isTrue())
    {
        auto itOuter = condPtsMap.find(var);
        if (itOuter != condPtsMap.end())
        {
            itOuter->second.erase(obj);
            if (itOuter->second.empty())
                condPtsMap.erase(itOuter);
        }
        return true;
    }

    auto itOuter = condPtsMap.find(var);
    if (itOuter != condPtsMap.end())
    {
        auto& map = itOuter->second;
        auto it = map.find(obj);
        if (it != map.end())
        {
            const PathCond* merged = PathCond::getOr(it->second, guard);
            if (merged == it->second)
                return false; // no change
            // If the merged result collapses to True, erase the entry.
            if (merged->isTrue())
            {
                map.erase(it);
                if (map.empty())
                    condPtsMap.erase(itOuter);
                return true;
            }
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
    CondPointsTo newMap;
    newMap.reserve(64);
    newMap[obj] = guard;
    condPtsMap[var] = std::move(newMap);
    return true;
}

/*!
 * Override SCC merge: synchronize conditional points-to and edge guards.
 */
bool ConditionalAndersen::mergeSrcToTgt(NodeID nodeId, NodeID newRepId)
{
    if (nodeId == newRepId)
        return false;
    double tStart = condProfile ? stat->getClk(true) : 0.0;

    // Merge condPtsMap: move all conditional pts from sub to rep.
    // In merge-cond-SCC mode, over-approximate guards to True.
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

    // Delegate to parent for actual edge moving / pts merging / node removal.
    // Edge-guard relocation is done in bulk after all merges in SCCDetect
    // to avoid O(N) linear scans inside every merge call.
    bool pwc = Andersen::mergeSrcToTgt(nodeId, newRepId);

    if (condProfile) timeCondSCCMerge += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return pwc;
}

/*!
 * Check whether an SCC (represented by its rep node) contains any
 * constraint edge with a non-trivial conditional guard.
 */
bool ConditionalAndersen::sccHasConditionalEdge(NodeID repId) const
{
    const NodeBS& subNodes = getSCCDetector()->subNodes(repId);
    for (NodeBS::iterator it = subNodes.begin(), eit = subNodes.end(); it != eit; ++it)
    {
        NodeID nodeId = *it;
        ConstraintNode* node = consCG->getConstraintNode(nodeId);
        // Outgoing edges
        for (ConstraintEdge* edge : node->getOutEdges())
        {
            if (edge->getEdgeKind() == ConstraintEdge::Copy ||
                edge->getEdgeKind() == ConstraintEdge::NormalGep ||
                edge->getEdgeKind() == ConstraintEdge::VariantGep)
            {
                const PathCond* g = edge->getGuard();
                if (g && !g->isTrue()) return true;
            }
        }
        // Incoming edges (also part of the SCC cycle)
        for (ConstraintEdge* edge : node->getInEdges())
        {
            if (edge->getEdgeKind() == ConstraintEdge::Copy ||
                edge->getEdgeKind() == ConstraintEdge::NormalGep ||
                edge->getEdgeKind() == ConstraintEdge::VariantGep)
            {
                const PathCond* g = edge->getGuard();
                if (g && !g->isTrue()) return true;
            }
        }
    }
    return false;
}

/*!
 * Override SCC detection: optionally skip merging SCCs that contain
 * conditional edges, preserving per-node conditional precision.
 */
NodeStack& ConditionalAndersen::SCCDetect()
{
    numOfSCCDetection++;

    double sccStart = stat->getClk(true);
    WPAConstraintSolver::SCCDetect();
    double sccEnd = stat->getClk(true);
    timeOfSCCDetection += (sccEnd - sccStart) / TIMEINTERVAL;

    double mergeStart = stat->getClk(true);

    NodeStack topoOrder = getSCCDetector()->topoNodeStack();
    std::unordered_map<NodeID, NodeID> mergedNodes;
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
            {
                mergeNodeToRep(subNodeId, repNodeId);
                mergedNodes[subNodeId] = repNodeId;
            }
        }
    }

    // Batch-update edgeGuards in a single pass to avoid O(N*M) linear scans.
    // Copy edge guards are stored on ConstraintEdge objects and are preserved
    // automatically by retargeting; only Load/Store/Gep guards need remapping.
    for (auto it = edgeGuards.begin(); it != edgeGuards.end(); )
    {
        const EdgeGuardKey& key = it->first;
        if (key.kind == CondEdgeKind::Copy) { ++it; continue; }
        auto srcIt = mergedNodes.find(key.src);
        auto dstIt = mergedNodes.find(key.dst);
        if (srcIt == mergedNodes.end() && dstIt == mergedNodes.end())
        {
            ++it;
            continue;
        }

        const PathCond* guard = it->second;
        NodeID newSrc = (srcIt != mergedNodes.end()) ? srcIt->second : key.src;
        NodeID newDst = (dstIt != mergedNodes.end()) ? dstIt->second : key.dst;
        EdgeGuardKey newKey{newSrc, newDst, key.kind};

        it = edgeGuards.erase(it);
        auto it2 = edgeGuards.find(newKey);
        if (it2 == edgeGuards.end())
            edgeGuards[newKey] = guard;
        else
            it2->second = PathCond::getOr(it2->second, guard);
    }

    double mergeEnd = stat->getClk(true);
    timeOfSCCMerges += (mergeEnd - mergeStart) / TIMEINTERVAL;

    return getSCCDetector()->topoNodeStack();
}
void ConditionalAndersen::solveWorklist()
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
        processNode(nodeId);
        collapseFields();
    }
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
        mergeCopyEdgeGuard(pair.first, pair.second, csGuard, false);
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
        mergeCopyEdgeGuard(pair.first, pair.second, csGuard, false);
    }

    cpySrcNodes.insert(newEdges.begin(), newEdges.end());
}

/*!
 * Process address edge: add unconditional (True, obj) to condPtsMap.
 */
void ConditionalAndersen::processAddr(const AddrCGEdge* addr)
{
    Andersen::processAddr(addr);
    // True guards are implicit in the bitvector points-to set.
    // No need to store them in condPtsMap.
}

/*!
 * Process copy edge: propagate conditional points-to with edge guard.
 */
bool ConditionalAndersen::processCopy(NodeID node, const ConstraintEdge* edge)
{
    if (kLimit == 0) return Andersen::processCopy(node, edge);
    bool parentChanged = Andersen::processCopy(node, edge);
    double tStart = condProfile ? stat->getClk(true) : 0.0;

    NodeID dst = edge->getDstID();
    const PathCond* guard = edge->getGuard();
    if (!guard) guard = PathCond::getTrue();

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

    // For non-True edge guards, also propagate objects whose source guard is
    // implicitly True (present in bitvector pts but absent from condPtsMap).
    if (!guard->isTrue())
    {
        const PathCond* limitedGuard = applyLimits(guard);
        if (!limitedGuard->isFalse() && !(eagerSat && !z3IsSat(limitedGuard)))
        {
            const PointsTo& pts = getPts(node);
            for (NodeID obj : pts)
            {
                if (it != condPtsMap.end() && it->second.count(obj)) continue;
                if (orMergeCondPts(dst, obj, limitedGuard))
                    condChanged = true;
            }
        }
    }

    if (condProfile) timeCondProp += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return parentChanged || condChanged;
}

/*!
 * Process load edge: create derived copy edge with guard.
 * Guard = load_edge_guard ∧ OR(conditions under which pointer points to node).
 */
bool ConditionalAndersen::processLoad(NodeID node, const ConstraintEdge* load)
{
    if (kLimit == 0) return Andersen::processLoad(node, load);
    bool parentChanged = Andersen::processLoad(node, load);
    double tStart = condProfile ? stat->getClk(true) : 0.0;

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
    const PathCond* guard;
    if (loadG->isTrue()) guard = ptsG;
    else if (ptsG->isTrue()) guard = loadG;
    else guard = PathCond::getAnd(loadG, ptsG);

    mergeCopyEdgeGuard(node, dst, guard, false);

    if (condProfile) timeCondProp += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return parentChanged;
}

/*!
 * Process store edge: create derived copy edge with guard.
 * Guard = store_edge_guard ∧ OR(conditions under which pointer points to node).
 */
bool ConditionalAndersen::processStore(NodeID node, const ConstraintEdge* store)
{
    if (kLimit == 0) return Andersen::processStore(node, store);
    bool parentChanged = Andersen::processStore(node, store);
    double tStart = condProfile ? stat->getClk(true) : 0.0;

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
    const PathCond* guard;
    if (storeG->isTrue()) guard = ptsG;
    else if (ptsG->isTrue()) guard = storeG;
    else guard = PathCond::getAnd(storeG, ptsG);

    mergeCopyEdgeGuard(src, node, guard, false);

    if (condProfile) timeCondProp += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return parentChanged;
}

/*!
 * Process gep edge: propagate conditional points-to with field translation.
 */
bool ConditionalAndersen::processGep(NodeID, const GepCGEdge* edge)
{
    if (kLimit == 0) return Andersen::processGep(edge->getSrcID(), edge);
    bool parentChanged = Andersen::processGep(edge->getSrcID(), edge);
    double tStart = condProfile ? stat->getClk(true) : 0.0;

    NodeID src = edge->getSrcID();
    NodeID dst = edge->getDstID();
    const PathCond* edgeG = edge->getGuard();
    if (!edgeG) edgeG = PathCond::getTrue();
    const bool edgeIsTrue = edgeG->isTrue();

    bool condChanged = false;
    bool isVariant = SVFUtil::isa<VariantGepCGEdge>(edge);
    const NormalGepCGEdge* normalGep =
        SVFUtil::dyn_cast<NormalGepCGEdge>(edge);
    assert((isVariant || normalGep) && "unknown gep edge kind");

    auto translateField = [&](NodeID o) -> NodeID
    {
        if (isVariant)
        {
            if (consCG->isBlkObjOrConstantObj(o))
                return o;
            if (!isFieldInsensitive(o))
            {
                setObjFieldInsensitive(o);
                consCG->addNodeToBeCollapsed(consCG->getBaseObjVarID(o));
            }
            return consCG->getFIObjVar(o);
        }
        else
        {
            if (consCG->isBlkObjOrConstantObj(o) || isFieldInsensitive(o))
                return o;
            return consCG->getGepObjVar(
                o, normalGep->getAccessPath().getConstantStructFldIdx());
        }
    };

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

            NodeID newField = translateField(o);
            if (orMergeCondPts(dst, newField, g))
                condChanged = true;
        }
    }

    // For non-True edge guards, also propagate objects whose source guard is
    // implicitly True (present in bitvector pts but absent from condPtsMap).
    if (!edgeIsTrue)
    {
        const PathCond* limitedEdgeG = applyLimits(edgeG);
        if (!limitedEdgeG->isFalse() && !(eagerSat && !z3IsSat(limitedEdgeG)))
        {
            const PointsTo& pts = getPts(src);
            for (NodeID o : pts)
            {
                if (it != condPtsMap.end() && it->second.count(o)) continue;
                NodeID newField = translateField(o);
                if (orMergeCondPts(dst, newField, limitedEdgeG))
                    condChanged = true;
            }
        }
    }

    // Do NOT push worklist on condChanged alone: condPtsMap does not affect
    // the underlying bitvector (ptD).  Worklist should only advance when the
    // bitvector changes (parentChanged), otherwise we introduce spurious
    // propagation that can alter the final fixpoint.
    if (condProfile) timeCondProp += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return parentChanged || condChanged;
}

/*!
 * Alias query using conditional points-to.
 * May-alias only if there exists a common object under a satisfiable conjunction.
 */
AliasResult ConditionalAndersen::alias(NodeID v1, NodeID v2)
{
    if (kLimit == 0) return Andersen::alias(v1, v2);
    double tStart = condProfile ? stat->getClk(true) : 0.0;
    numAliasTotal++;
    if (v1 == v2) return MustAlias;

    NodeID n1 = consCG->sccRepNode(v1);
    NodeID n2 = consCG->sccRepNode(v2);

    // Fast path: no unconditional overlap -> NoAlias.
    if (!BVDataPTAImpl::alias(getPts(n1), getPts(n2)))
        return AliasResult::NoAlias;

    // Build expanded conditional pts with implicit True guards.
    CondPointsTo pts1 = expandCondFIObjs(v1);
    CondPointsTo pts2 = expandCondFIObjs(v2);

    // If either side has no conditional entries at all, all common objects
    // have at least one True guard, so the conjunction is satisfiable.
    if (pts1.empty() || pts2.empty())
    {
        if (condProfile) timeCondAlias += (stat->getClk(true) - tStart) / TIMEINTERVAL;
        return AliasResult::MayAlias;
    }

    for (const auto& p1 : pts1)
    {
        auto it2 = pts2.find(p1.first);
        if (it2 == pts2.end()) continue;
        const PathCond* conj = PathCond::getAnd(p1.second, it2->second);
        if (z3IsSat(conj))
        {
            numAliasRefined++;
            if (condProfile) timeCondAlias += (stat->getClk(true) - tStart) / TIMEINTERVAL;
            return AliasResult::MayAlias;
        }
    }

    numAliasRefinedToNoAlias++;
    if (condProfile) timeCondAlias += (stat->getClk(true) - tStart) / TIMEINTERVAL;
    return AliasResult::NoAlias;
}

AliasResult ConditionalAndersen::alias(const SVFVar* v1, const SVFVar* v2)
{
    if (v1->getId() == v2->getId()) return MustAlias;
    return alias(v1->getId(), v2->getId());
}

/*!
 * Expand field-insensitive objects in a conditional points-to set.
 * True guards are implicit: objects in the bitvector pts but absent from
 * condPtsMap are treated as having guard True.
 */
ConditionalAndersen::CondPointsTo ConditionalAndersen::expandCondFIObjs(NodeID nodeId) const
{
    CondPointsTo expanded;
    NodeID rep = consCG->sccRepNode(nodeId);
    const PointsTo& pts = getPts(rep);
    expanded.reserve(pts.count());
    const CondPointsTo& condPts = getCondPts(nodeId);

    for (NodeID obj : pts)
    {
        const PathCond* guard = PathCond::getTrue();
        auto it = condPts.find(obj);
        if (it != condPts.end())
            guard = it->second;

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
                auto jt = expanded.find(f);
                if (jt == expanded.end())
                    expanded[f] = guard;
                else
                    jt->second = PathCond::getOr(jt->second, guard);
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
    SVFUtil::outs() << "--- Copy Guards ---\n";
    for (ConstraintEdge* edge : consCG->getDirectCGEdges())
    {
        if (edge->getEdgeKind() == ConstraintEdge::Copy)
        {
            const PathCond* g = edge->getGuard();
            if (g && !g->isTrue())
            {
                SVFUtil::outs() << "  (" << edge->getSrcID() << " -> "
                                << edge->getDstID() << "): "
                                << g->toString() << "\n";
            }
        }
    }
    dumpByKind(CondEdgeKind::Load, "Load Guards");
    dumpByKind(CondEdgeKind::Store, "Store Guards");
    dumpByKind(CondEdgeKind::Gep, "GEP Guards");
    SVFUtil::outs() << "=====================================================\n\n";
}

/*!
 * Sample alias queries on top-level pointers to measure precision gain.
 * Randomly selects pairs of top-level ValVars and queries alias().
 * Statistics are accumulated into numAliasTotal / numAliasRefined /
 * numAliasRefinedToNoAlias / timeCondAlias.
 */
void ConditionalAndersen::sampleAliasQueries(u32_t sampleSize)
{
    // Collect all top-level ValVar IDs.
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

    std::mt19937 rng(42); // fixed seed for reproducibility
    std::uniform_int_distribution<size_t> dist(0, topVars.size() - 1);

    SVFUtil::outs() << "  [sampleAlias] Sampling " << sampleSize
                    << " pairs from " << topVars.size() << " top-level vars...\n";

    for (u32_t i = 0; i < sampleSize; ++i)
    {
        size_t a = dist(rng);
        size_t b = dist(rng);
        if (a == b) { if (++b >= topVars.size()) b = 0; }
        alias(topVars[a], topVars[b]);
    }

    u32_t baseMay = numAliasRefined + numAliasRefinedToNoAlias;
    SVFUtil::outs() << "  [sampleAlias] Done. baseMayAlias=" << baseMay
                    << " refinedToNoAlias=" << numAliasRefinedToNoAlias
                    << " stayedMayAlias=" << numAliasRefined << "\n";
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

    // Run sampled alias queries to measure precision gain vs base Andersen.
    // Only run when kLimit > 0 (otherwise identical to base Andersen).
    if (kLimit != 0)
        sampleAliasQueries(10000);

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
    SVFUtil::outs() << "  Alias base MayAlias: " << (numAliasRefined + numAliasRefinedToNoAlias) << "\n";
    SVFUtil::outs() << "  Alias refined (May): " << numAliasRefined << "\n";
    SVFUtil::outs() << "  Alias refined (No):  " << numAliasRefinedToNoAlias << "\n";
    SVFUtil::outs() << "  CondPts entries:     " << numCondPtsEntries << "\n";
    SVFUtil::outs() << "\n  === Conditional Overhead (ms) ===\n";
    SVFUtil::outs() << "  Cond propagation:    " << timeCondProp << "\n";
    SVFUtil::outs() << "  Cond alias query:    " << timeCondAlias << "\n";
    SVFUtil::outs() << "  Cond SCC merge:      " << timeCondSCCMerge << "\n";
    SVFUtil::outs() << "  Guard limit:         " << timeGuardLimit << "\n";
    SVFUtil::outs() << "  SAT check:           " << timeSATCheck << "\n";
    SVFUtil::outs() << "  Total cond overhead: " << (timeCondProp + timeCondAlias + timeCondSCCMerge + timeGuardLimit + timeSATCheck) << "\n";
    size_t copyGuardCount = 0, loadGuardCount = 0, storeGuardCount = 0, gepGuardCount = 0;
    for (ConstraintEdge* edge : consCG->getDirectCGEdges())
    {
        const PathCond* g = edge->getGuard();
        if (!g || g->isTrue()) continue;
        auto kind = edge->getEdgeKind();
        if (kind == ConstraintEdge::Copy)
            ++copyGuardCount;
        else if (kind == ConstraintEdge::NormalGep || kind == ConstraintEdge::VariantGep)
            ++gepGuardCount;
    }
    for (ConstraintEdge* edge : consCG->getLoadCGEdges())
    {
        const PathCond* g = edge->getGuard();
        if (g && !g->isTrue()) ++loadGuardCount;
    }
    for (ConstraintEdge* edge : consCG->getStoreCGEdges())
    {
        const PathCond* g = edge->getGuard();
        if (g && !g->isTrue()) ++storeGuardCount;
    }
    SVFUtil::outs() << "  Copy guards:         " << copyGuardCount << "\n";
    SVFUtil::outs() << "  Load guards:         " << loadGuardCount << "\n";
    SVFUtil::outs() << "  Store guards:        " << storeGuardCount << "\n";
    SVFUtil::outs() << "  GEP guards:          " << gepGuardCount << "\n";
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
    NodeID rep = consCG->sccRepNode(id);
    auto it = condPtsMap.find(rep);
    if (it != condPtsMap.end())
        return it->second;

    static CondPointsTo empty;
    return empty;
}
