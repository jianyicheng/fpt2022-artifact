#include "llvm/ADT/APInt.h"
#include "llvm/IR/Metadata.h"

#include "ElasticPass/CircuitGenerator.h"
#include "ElasticPass/DASS/DASSUtils.h"
#include "ElasticPass/Utils.h"

#include <algorithm>

//--------------------------------------------------------//
// Function: optimizeIfStmt
// Analyse if statement. If it does not touch memory, then directly forward its control branch
// signal from the condition block to the merged block.
//--------------------------------------------------------//

struct IfBlock {
    BasicBlock *condBB, *trueStart, *trueEnd, *falseStart, *falseEnd, *exitBB;

    IfBlock(BasicBlock* condB, BasicBlock* trueB, BasicBlock* falseB, BasicBlock* exitB) {
        condBB     = condB;
        trueStart  = trueB;
        trueEnd    = trueB;
        falseStart = falseB;
        falseEnd   = falseB;
        exitBB     = exitB;
    }
};

static bool touchMemory(BasicBlock* BB, ENode_vec* enode_dag) {
    if (!BB)
        return false;
    for (auto enode : *enode_dag)
        if (enode->Instr && enode->type == Inst_ && enode->BB == BB)
            if (isa<LoadInst>(enode->Instr) || isa<StoreInst>(enode->Instr))
                return true;

    return false;
}

static std::vector<IfBlock*> getInnerMostIfBlocks(Function* F, ENode_vec* enode_dag) {
    std::vector<IfBlock*> ifBlocks;
    for (auto BB = F->begin(); BB != F->end(); BB++) {
        auto branchInst = dyn_cast<BranchInst>(BB->getTerminator());
        if (!branchInst || !branchInst->isConditional())
            continue;

        auto bb0 = branchInst->getSuccessor(0);
        auto bb1 = branchInst->getSuccessor(1);
        auto br0 = dyn_cast<BranchInst>(bb0->getTerminator());
        auto br1 = dyn_cast<BranchInst>(bb1->getTerminator());

        // TODO: What if the exit block terminates with ret?
        if (!br0 || !br1)
            continue;

        // We are looking for inner most if statements, which has the structure of:
        // BB1 -> BB2 (+ BB3) -> BB4. At least one of the successor should have unconditional branch
        if (br0->isConditional() && br1->isConditional())
            continue;
        if (!br0->isConditional() && !br1->isConditional() &&
            br0->getSuccessor(0) != br1->getSuccessor(0))
            continue;

        BasicBlock *trueBB, *falseBB, *exitBB;
        if (!br0->isConditional()) {
            trueBB = bb0;
            exitBB = br0->getSuccessor(0);
            if (br1->isConditional() && exitBB != bb1)
                continue;
            if (!br1->isConditional() && br1->getSuccessor(0) != exitBB)
                continue;
            if (!br1->isConditional() && br1->getSuccessor(0) == exitBB)
                falseBB = bb1;
        } else {
            exitBB = bb0;
            if (br1->getSuccessor(0) != exitBB)
                continue;
            falseBB = bb1;
        }
        if (touchMemory(trueBB, enode_dag) || touchMemory(falseBB, enode_dag))
            continue;

        IfBlock* ifBlock = new IfBlock(&*BB, trueBB, falseBB, exitBB);
        ifBlocks.push_back(ifBlock);
    }
    return ifBlocks;
}

static bool isLegalToDestroy(ENode* enode, IfBlock* ifBlock) {
    for (auto succ : *enode->CntrlSuccs) {
        if (ifBlock->exitBB == succ->BB)
            continue;

        if (succ->type == Sink_)
            return false;

        if (succ->BB != ifBlock->trueStart && succ->BB != ifBlock->falseStart) {
            llvm::errs() << getNodeDotNameNew(succ) << "\n";
            llvm_unreachable(" does not match any of the block successors.");
        }

        if (succ->type == Phi_n && succ->CntrlSuccs->size() == 1) {
            auto nextBranch = succ->CntrlSuccs->at(0);
            if (nextBranch->type == Branch_n && (nextBranch->CntrlSuccs->at(0)->type == Sink_ ||
                                                 nextBranch->CntrlSuccs->at(1)->type == Sink_)) {
                auto exitPhi = (nextBranch->CntrlSuccs->at(0)->type == Sink_)
                                   ? nextBranch->CntrlSuccs->at(1)
                                   : nextBranch->CntrlSuccs->at(0);
                if (exitPhi->type == Phi_n && exitPhi->BB == ifBlock->exitBB)
                    continue;
            }
        }
        return false;
    }
    return true;
}

static ENode* createShortPathIf(IfBlock* ifBlock, ENode_vec* enode_dag, BBNode_vec* bbnode_dag) {

    auto BB = ifBlock->condBB;
    // Get control phi at the conditional block
    auto phiC = getPhiC(BB, enode_dag);
    assert(phiC->JustCntrlSuccs->size() > 0);
    phiC = insertFork(phiC, enode_dag, true);

    auto condBranch = getBranchC(BB, enode_dag);
    assert(condBranch->JustCntrlPreds->size() == 2);
    auto cond0 = condBranch->JustCntrlPreds->at(0);
    if (cond0->type == Fork_c || cond0->type == Fork_)
        cond0 = (cond0->CntrlPreds->size() > 0) ? cond0->CntrlPreds->at(0)
                                                : cond0->JustCntrlPreds->at(0);
    auto cond1 = condBranch->JustCntrlPreds->at(1);
    if (cond1->type == Fork_c || cond1->type == Fork_)
        cond1 = (cond1->CntrlPreds->size() > 0) ? cond1->CntrlPreds->at(0)
                                                : cond1->JustCntrlPreds->at(0);
    assert(cond0->type == Branch_ || cond1->type == Branch_);
    auto cond = (cond0->type == Branch_) ? condBranch->JustCntrlPreds->at(0)
                                         : condBranch->JustCntrlPreds->at(1);

    // Create a short path driver
    auto cstNode         = new ENode(Branch_, "brCst", phiC->BB);
    cstNode->bbNode      = phiC->bbNode;
    cstNode->id          = enode_dag->size();
    cstNode->bbId        = phiC->bbId;
    cstNode->isShortPath = true;
    enode_dag->push_back(cstNode);
    cstNode->JustCntrlPreds->push_back(phiC);
    phiC->JustCntrlSuccs->push_back(cstNode);
    cstNode = insertFork(cstNode, enode_dag, false);

    // Create a short path control branch
    auto branchC    = new ENode(Branch_c, "branchC", phiC->BB);
    branchC->bbNode = phiC->bbNode;
    branchC->id     = enode_dag->size();
    branchC->bbId   = phiC->bbId;
    enode_dag->push_back(branchC);
    branchC->condition = cstNode;
    branchC->CntrlPreds->push_back(cstNode);
    cstNode->CntrlSuccs->push_back(branchC);
    branchC->JustCntrlPreds->push_back(phiC);
    phiC->JustCntrlSuccs->push_back(branchC);
    // One constant sink dummy
    ENode* branchSinkNode = new ENode(Sink_, "sink");
    branchSinkNode->id    = enode_dag->size();
    branchSinkNode->bbId  = 0;
    enode_dag->push_back(branchSinkNode);
    branchSinkNode->JustCntrlPreds->push_back(branchC);
    branchC->JustCntrlSuccs->push_back(branchSinkNode);
    branchC->branchFalseOutputSucc = branchSinkNode;

    // Get the control phi at the exit block
    auto exitPhiC = getPhiC(ifBlock->exitBB, enode_dag);
    assert(exitPhiC->CntrlSuccs->size() <= 1);

    // Create a short path decision branch to synchronize the sequence from two branches
    if (exitPhiC->CntrlSuccs->size() == 1) {
        auto selectBranch       = new ENode(Branch_n, "branch", phiC->BB);
        selectBranch->bbNode    = phiC->bbNode;
        selectBranch->id        = enode_dag->size();
        selectBranch->bbId      = phiC->bbId;
        selectBranch->condition = cstNode;
        enode_dag->push_back(selectBranch);
        selectBranch->CntrlPreds->push_back(cstNode);
        cstNode->CntrlSuccs->push_back(selectBranch);
        selectBranch->CntrlPreds->push_back(cond);
        cond->CntrlSuccs->push_back(selectBranch);
        exitPhiC->CntrlSuccs->at(0)->CntrlPreds->push_back(selectBranch);
        selectBranch->CntrlSuccs->push_back(exitPhiC->CntrlSuccs->at(0));
        selectBranch->branchTrueOutputSucc = exitPhiC->CntrlSuccs->at(0);
        // One constant sink dummy
        ENode* selectBranchSinkNode = new ENode(Sink_, "sink");
        selectBranchSinkNode->id    = enode_dag->size();
        selectBranchSinkNode->bbId  = 0;
        enode_dag->push_back(selectBranchSinkNode);
        selectBranchSinkNode->CntrlPreds->push_back(selectBranch);
        selectBranch->CntrlSuccs->push_back(selectBranchSinkNode);
        selectBranch->branchFalseOutputSucc = selectBranchSinkNode;
    }

    // Create a new control phi at the exit block and take over the control output
    auto newPhiC       = new ENode(Phi_c, "phiC", exitPhiC->BB);
    newPhiC->isCntrlMg = false;
    newPhiC->bbNode    = exitPhiC->bbNode;
    newPhiC->id        = enode_dag->size();
    newPhiC->bbId      = exitPhiC->bbId;
    enode_dag->push_back(newPhiC);
    newPhiC->JustCntrlSuccs =
        new ENode_vec(exitPhiC->JustCntrlSuccs->begin(), exitPhiC->JustCntrlSuccs->end());
    exitPhiC->JustCntrlSuccs->clear();
    for (auto succ : *newPhiC->JustCntrlSuccs) {
        auto iter = std::find(succ->JustCntrlPreds->begin(), succ->JustCntrlPreds->end(), exitPhiC);
        if (iter != succ->JustCntrlPreds->end()) {
            succ->JustCntrlPreds->erase(iter);
            succ->JustCntrlPreds->push_back(newPhiC);
        }
        iter = std::find(succ->CntrlPreds->begin(), succ->CntrlPreds->end(), exitPhiC);
        if (iter != succ->CntrlPreds->end()) {
            succ->CntrlPreds->erase(iter);
            succ->CntrlPreds->push_back(newPhiC);
        }
    }
    newPhiC->JustCntrlPreds->push_back(branchC);
    branchC->JustCntrlSuccs->push_back(newPhiC);
    branchC->branchTrueOutputSucc = newPhiC;

    eraseNode(exitPhiC, enode_dag);
    // Remove the branchC in both branches - as we directly forward the control signal from the
    // conditional block to the exit block
    if (ifBlock->trueStart) {
        auto trueBranch = getBranchC(ifBlock->trueStart, enode_dag);
        eraseNode(trueBranch, enode_dag);
    }
    if (ifBlock->falseStart) {
        auto falseBranch = getBranchC(ifBlock->falseStart, enode_dag);
        eraseNode(falseBranch, enode_dag);
    }

    return cstNode;
}

static void replaceWithShortCut(ENode* branchNode, ENode* brCst, ENode_vec* enode_dag,
                                BasicBlock* exitBB) {
    ENode* select;
    for (auto pred : *branchNode->CntrlPreds) {
        auto in = (pred->type == Fork_) ? pred->CntrlPreds->at(0) : pred;
        if (in->type == Branch_ || (in->type == Inst_ && isa<CmpInst>(in->Instr))) {
            select = pred;
            break;
        }
    }
    assert(select);

    branchNode->CntrlPreds->erase(
        std::find(branchNode->CntrlPreds->begin(), branchNode->CntrlPreds->end(), select));
    select->CntrlSuccs->erase(
        std::find(select->CntrlSuccs->begin(), select->CntrlSuccs->end(), branchNode));
    branchNode->CntrlPreds->push_back(brCst);
    brCst->CntrlSuccs->push_back(branchNode);

    ENode* exitPhi;
    for (auto succ : *branchNode->CntrlSuccs) {
        if (exitBB == succ->BB)
            continue;

        auto phi    = succ;
        auto branch = phi->CntrlSuccs->at(0);
        if (branch->CntrlSuccs->at(0)->type == Sink_)
            exitPhi = branch->CntrlSuccs->at(1);
        else
            exitPhi = branch->CntrlSuccs->at(0);

        eraseNode(phi, enode_dag);
        eraseNode(branch, enode_dag);
    }

    ENode* branchSinkNode = new ENode(Sink_, "sink");
    branchSinkNode->id    = enode_dag->size();
    branchSinkNode->bbId  = 0;
    enode_dag->push_back(branchSinkNode);

    branchNode->CntrlSuccs->push_back(branchSinkNode);
    branchSinkNode->CntrlPreds->push_back(branchNode);

    if (branchNode->CntrlSuccs->size() == 2)
        return;

    branchNode->CntrlSuccs->push_back(exitPhi);
    exitPhi->CntrlPreds->push_back(branchNode);
    assert(exitPhi->CntrlPreds->size() == 1);
    return;
}

static void optimizeIfBlock(IfBlock* ifBlock, ENode_vec* enode_dag, BBNode_vec* bbnode_dag) {
    // Check if there is any values are directly forwarded through both branches
    ENode_vec branchNodes;
    auto condBB = ifBlock->condBB;
    for (auto enode : *enode_dag)
        if (condBB == enode->BB && enode->type == Branch_n && isLegalToDestroy(enode, ifBlock))
            branchNodes.push_back(enode);

    if (branchNodes.size() == 0)
        return;

    // Construct another branchC node in condBB and another phiC node in exitBB
    auto brCst  = createShortPathIf(ifBlock, enode_dag, bbnode_dag);
    auto exitBB = ifBlock->exitBB;
    for (auto branchNode : branchNodes)
        replaceWithShortCut(branchNode, brCst, enode_dag, exitBB);
}

// TODO: To support multi-depth if statements
void CircuitGenerator::optimizeIfStmt(Function* F) {
    auto ifBlocks = getInnerMostIfBlocks(F, enode_dag);
    for (auto ifBlock : ifBlocks)
        optimizeIfBlock(ifBlock, enode_dag, bbnode_dag);
}

//--------------------------------------------------------//
// Function: optimizeLoops
// Analyze and optimize the loops so loop invariants are replaced with mux to enable loop level
// pipelining
//--------------------------------------------------------//

static bool containsCallInst(Loop* loop) {
    for (auto BB : loop->getBlocks())
        for (auto I = BB->begin(); I != BB->end(); I++)
            if (isa<CallInst>(I))
                return true;
    return false;
}

static bool hasLoopInvar(Loop* loop, ENode_vec* enode_dag) {
    for (auto enode : *enode_dag)
        if (enode->BB == loop->getHeader() && enode->type == Phi_n)
            return true;
    return false;
}

static void createShortPathLoop(ENode* enode, ENode* cstNode, Loop* loop, ENode_vec* enode_dag) {
    assert(enode->CntrlPreds->size() == 2);
    ENode *entryBranch, *exitBranch, *exitPhi, *dataIn;
    if (enode->CntrlPreds->at(0)->BB == loop->getExitingBlock()) {
        entryBranch = enode->CntrlPreds->at(1);
        exitBranch  = enode->CntrlPreds->at(0);
    } else if (enode->CntrlPreds->at(1)->BB == loop->getExitingBlock()) {
        entryBranch = enode->CntrlPreds->at(0);
        exitBranch  = enode->CntrlPreds->at(1);
    } else {
        llvm::errs() << getNodeDotNameNew(enode) << "\n";
        llvm_unreachable("Cannot find the back edge for the loop.");
    }
    assert(exitBranch->CntrlSuccs->size() == 2);
    assert(exitBranch->BB == loop->getExitingBlock());
    if (exitBranch->CntrlSuccs->at(0) == enode)
        exitPhi = exitBranch->CntrlSuccs->at(1);
    else if (exitBranch->CntrlSuccs->at(1) == enode)
        exitPhi = exitBranch->CntrlSuccs->at(0);
    else {
        llvm::errs() << getNodeDotNameNew(enode) << "\n";
        llvm_unreachable("Cannot find the exit edge for the loop.");
    }
    assert(exitPhi->BB == loop->getExitBlock());
    assert(exitPhi->CntrlPreds->size() == 1);

    auto branchIn0 = entryBranch->CntrlPreds->at(0);
    branchIn0      = (branchIn0->type == Fork_) ? branchIn0->CntrlPreds->at(0) : branchIn0;
    auto branchIn1 = entryBranch->CntrlPreds->at(1);
    branchIn1      = (branchIn1->type == Fork_) ? branchIn1->CntrlPreds->at(0) : branchIn1;

    if ((branchIn0->type == Branch_) == (branchIn1->type == Branch_)) {
        llvm::errs() << getNodeDotNameNew(entryBranch) << " -> " << getNodeDotNameNew(enode)
                     << "\n";
        llvm_unreachable("Cannot find the input data for the invariant.");
    }
    if (branchIn0->type == Branch_)
        dataIn = entryBranch->CntrlPreds->at(1);
    else if (branchIn1->type == Branch_)
        dataIn = entryBranch->CntrlPreds->at(0);
    dataIn     = insertFork(dataIn, enode_dag, false);

    // One constant sink dummy for exit branch
    ENode* exitBranchSinkNode = new ENode(Sink_, "sink");
    exitBranchSinkNode->id    = enode_dag->size();
    exitBranchSinkNode->bbId  = 0;
    enode_dag->push_back(exitBranchSinkNode);
    exitBranchSinkNode->CntrlPreds->push_back(exitBranch);
    exitBranch->CntrlSuccs->erase(
        std::find(exitBranch->CntrlSuccs->begin(), exitBranch->CntrlSuccs->end(), exitPhi));
    exitPhi->CntrlPreds->clear();
    exitBranch->CntrlSuccs->push_back(exitBranchSinkNode);
    exitBranch->branchFalseOutputSucc = exitBranchSinkNode;

    auto selectBranch       = new ENode(Branch_n, "branch", cstNode->BB);
    selectBranch->bbNode    = cstNode->bbNode;
    selectBranch->id        = enode_dag->size();
    selectBranch->bbId      = cstNode->bbId;
    selectBranch->condition = cstNode;
    enode_dag->push_back(selectBranch);
    selectBranch->CntrlPreds->push_back(cstNode);
    cstNode->CntrlSuccs->push_back(selectBranch);
    selectBranch->CntrlPreds->push_back(dataIn);
    dataIn->CntrlSuccs->push_back(selectBranch);
    exitPhi->CntrlPreds->push_back(selectBranch);
    selectBranch->CntrlSuccs->push_back(exitPhi);
    selectBranch->branchTrueOutputSucc = exitPhi;
    // One constant sink dummy for select branch
    ENode* selectBranchSinkNode = new ENode(Sink_, "sink");
    selectBranchSinkNode->id    = enode_dag->size();
    selectBranchSinkNode->bbId  = 0;
    enode_dag->push_back(selectBranchSinkNode);
    selectBranchSinkNode->CntrlPreds->push_back(selectBranch);
    selectBranch->CntrlSuccs->push_back(selectBranchSinkNode);
    selectBranch->branchFalseOutputSucc = selectBranchSinkNode;
}

static void replaceMergeWithMux(ENode* enode, ENode* cntrlMerge, Loop* loop, ENode_vec* enode_dag,
                                int& id) {

    enode->type  = Phi_;
    enode->isMux = true;
    enode->id    = ++id;
    enode->CntrlPreds->push_back(cntrlMerge);
    cntrlMerge->CntrlSuccs->push_back(enode);
}

static void optimizeLoop(Loop* loop, ENode_vec* enode_dag, BBNode_vec* bbnode_dag) {

    if (hasLoopInvar(loop, enode_dag)) {

        auto entryBlock = loop->getLoopPreheader();
        auto exitBlock  = loop->getExitBlock();
        auto header     = loop->getHeader();

        auto entryBBNode = getBBNode(entryBlock, bbnode_dag);
        auto exitBBNode  = getBBNode(exitBlock, bbnode_dag);
        assert(entryBBNode && exitBBNode);
        entryBBNode->CntrlSuccs->push_back(exitBBNode);
        exitBBNode->CntrlPreds->push_back(entryBBNode);

        auto brCst = getBrCst(entryBlock, enode_dag);
        if (!brCst) {
            auto phiC = getPhiC(entryBlock, enode_dag);
            assert(phiC->JustCntrlSuccs->size() > 0);
            phiC = insertFork(phiC, enode_dag, true);

            // Create a short path driver
            auto cstNode         = new ENode(Branch_, "brCstSP", phiC->BB);
            cstNode->bbNode      = phiC->bbNode;
            cstNode->id          = enode_dag->size();
            cstNode->bbId        = phiC->bbId;
            cstNode->isShortPath = true;
            enode_dag->push_back(cstNode);
            cstNode->JustCntrlPreds->push_back(phiC);
            phiC->JustCntrlSuccs->push_back(cstNode);
            brCst = cstNode;
        }
        brCst = insertFork(brCst, enode_dag, false);

        // Get the control merge op that controls the mux ops
        auto phiC = getPhiC(header, enode_dag);
        assert(phiC->JustCntrlSuccs->size() > 0);
        phiC = insertFork(phiC, enode_dag, false);

        // Get the maximum Phi ID
        int id = 0;
        for (auto enode : *enode_dag)
            if (enode->type == Phi_)
                id = std::max(id, enode->id);

        for (auto enode : *enode_dag)
            if (enode->BB == header && enode->type == Phi_n) {
                // 1. Create a short control path for all the loop invariants
                createShortPathLoop(enode, brCst, loop, enode_dag);
                // 2. Replace all the merge ops to mux ops to enable parallelism
                replaceMergeWithMux(enode, phiC, loop, enode_dag, id);
            }
    }
}

static void extractInnermostLoop(Loop* loop, std::vector<Loop*>& innermostLoops) {
    auto subLoops = loop->getSubLoops();
    if (subLoops.empty())
        innermostLoops.push_back(loop);
    else
        for (auto subloop : subLoops)
            extractInnermostLoop(subloop, innermostLoops);
}

static std::vector<Loop*> extractInnermostLoops(LoopInfo& LI) {
    std::vector<Loop*> innermostLoops;
    for (auto& loop : LI)
        extractInnermostLoop(loop, innermostLoops);
    return innermostLoops;
}

void CircuitGenerator::optimizeLoops(Function* F) {
    auto DT = llvm::DominatorTree(*F);
    LoopInfo LI(DT);

    auto innermostLoops = extractInnermostLoops(LI);
    for (auto& loop : innermostLoops) {
        // Skip loops that have function calls or multiple back edges - cannot analyze function
        if (containsCallInst(loop) || loop->getNumBackEdges() != 1)
            continue;
        optimizeLoop(loop, enode_dag, bbnode_dag);
    }
}

//--------------------------------------------------------//
// Function: insertLoopInterchangers
// Analyze loop and insert a loop interchanger if it is specified.
//--------------------------------------------------------//

int getLoopInterchangeDepth(Loop* loop) {
    auto id              = loop->getLoopID();
    LLVMContext& Context = loop->getHeader()->getContext();
    for (unsigned int i = 0; i < id->getNumOperands(); i++)
        if (auto node = dyn_cast<MDNode>(id->getOperand(i))) {
            Metadata* arg = node->getOperand(0);
            if (arg == MDString::get(Context, "dass.loop.interchange")) {
                Metadata* depth                  = node->getOperand(1);
                ConstantAsMetadata* depthAsValue = dyn_cast<ConstantAsMetadata>(depth);
                if (depthAsValue)
                    return depthAsValue->getValue()->getUniqueInteger().getSExtValue();
            }
        }
    return -1;
}

std::string insertLoopInterchanger(Loop* loop, ENode_vec* enode_dag) {
    auto depth = getLoopInterchangeDepth(loop);
    if (depth == -1)
        return "";

    auto entryBanchC = getBranchC(loop->getLoopPreheader(), enode_dag);
    auto entryPhiC   = getPhiC(loop->getHeader(), enode_dag);
    auto exitBranchC = getBranchC(loop->getHeader(), enode_dag);
    auto exitPhiC    = getPhiC(loop->getExitBlock(), enode_dag);
    return getNodeDotNameNew(entryBanchC) + ", " + getNodeDotNameNew(exitBranchC) + ", " +
           getNodeDotNameNew(entryPhiC) + ", " + getNodeDotNameNew(exitPhiC) + ", " +
           std::to_string(depth) + "\n";
}

void CircuitGenerator::insertLoopInterchangers(Function* F) {
    std::error_code ec;
    llvm::raw_fd_ostream outfile("./loop_interchange.tcl", ec);

    auto DT = llvm::DominatorTree(*F);
    LoopInfo LI(DT);
    for (auto loop : LI) {
        outfile << insertLoopInterchanger(loop, enode_dag);
        auto subLoops = loop->getSubLoops();
        if (!subLoops.empty())
            for (auto subloop : subLoops)
                outfile << insertLoopInterchanger(subloop, enode_dag);
    }
    outfile.close();
}