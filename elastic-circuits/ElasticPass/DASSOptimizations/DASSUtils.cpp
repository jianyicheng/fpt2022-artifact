#include "DASS/DASSUtils.h"
#include "Nodes.h"

#include <vector>

void printEnodeVector(ENode_vec* enode_dag, std::string str) {
    llvm::errs() << str << "\n";
    for (size_t idx = 0; idx < enode_dag->size(); idx++) {
        auto& enode = (*enode_dag)[idx];
        llvm::errs() << enode->Name << "_" << std::to_string(enode->id) << "\n";
    }
}

std::string demangleFuncName(const char* name) {
    auto status = -1;

    std::unique_ptr<char, void (*)(void*)> res{abi::__cxa_demangle(name, NULL, NULL, &status),
                                               std::free};
    auto func = (status == 0) ? res.get() : std::string(name);
    return func.substr(0, func.find("("));
}

BBNode* getBBNode(BasicBlock* BB, BBNode_vec* bbnode_dag) {
    for (auto bbnode : *bbnode_dag)
        if (bbnode->BB == BB)
            return bbnode;
    return nullptr;
}

// Append a fork node after enode
ENode* insertFork(ENode* enode, ENode_vec* enode_dag, bool isControl) {
    if (enode->JustCntrlSuccs->size() > 0 && isControl &&
        enode->JustCntrlSuccs->at(0)->type == Fork_c)
        return enode->JustCntrlSuccs->at(0);
    if (enode->CntrlSuccs->size() > 0 && !isControl && enode->CntrlSuccs->at(0)->type == Fork_)
        return enode->CntrlSuccs->at(0);
    if ((!isControl && enode->type == Fork_) || (isControl && enode->type == Fork_c))
        return enode;

    ENode* forkNode;
    if (isControl) {
        forkNode         = new ENode(Fork_c, "forkC", enode->BB);
        forkNode->bbNode = enode->bbNode;
        forkNode->id     = enode_dag->size();
        forkNode->bbId   = enode->bbId;

        if (enode->JustCntrlSuccs->size() > 0)
            forkNode->JustCntrlSuccs =
                new ENode_vec(enode->JustCntrlSuccs->begin(), enode->JustCntrlSuccs->end());
        forkNode->JustCntrlPreds->push_back(enode);

        enode->JustCntrlSuccs->clear();
        enode->JustCntrlSuccs->push_back(forkNode);
    } else {
        forkNode         = new ENode(Fork_, "fork", enode->BB);
        forkNode->bbNode = enode->bbNode;
        forkNode->id     = enode_dag->size();
        forkNode->bbId   = enode->bbId;

        if (enode->CntrlSuccs->size() > 0)
            forkNode->CntrlSuccs =
                new ENode_vec(enode->CntrlSuccs->begin(), enode->CntrlSuccs->end());
        forkNode->CntrlPreds->push_back(enode);

        enode->CntrlSuccs->clear();
        enode->CntrlSuccs->push_back(forkNode);
    }
    enode_dag->push_back(forkNode);
    return forkNode;
}

ENode* getPhiC(BasicBlock* BB, ENode_vec* enode_dag) {
    ENode* phiCNode;
    auto count = 0;
    for (auto enode : *enode_dag)
        if (enode->BB == BB && enode->type == Phi_c) {
            phiCNode = enode;
            count++;
        }
    if (count != 1) {
        llvm::errs() << count << "\n" << *BB << "\n";
        llvm_unreachable("Cannot find the unique PhiC.");
    }

    return phiCNode;
}

ENode* getCallNode(CallInst* callInst, ENode_vec* enode_dag) {
    ENode* callNode;
    auto count = 0;
    for (auto enode : *enode_dag)
        if (enode->type == Inst_)
            if (enode->Instr == callInst) {
                callNode = enode;
                count++;
            }
    if (count != 1) {
        llvm::errs() << count << "\n" << *callInst << "\n";
        llvm_unreachable("Cannot find the unique call node.");
    }
    return callNode;
}

ENode* getBrCst(BasicBlock* BB, ENode_vec* enode_dag) {
    ENode* cstBr = nullptr;
    auto count   = 0;
    for (auto enode : *enode_dag)
        if (enode->BB == BB && enode->type == Branch_) {
            cstBr = enode;
            count++;
        }
    if (count > 1) {
        llvm::errs() << count << "\n" << *BB << "\n";
        llvm_unreachable("Cannot find the unique PhiC.");
    }

    return cstBr;
}

ENode* getBranchC(BasicBlock* BB, ENode_vec* enode_dag) {
    for (auto enode : *enode_dag)
        if (enode->BB == BB && enode->type == Branch_c)
            return enode;
    return nullptr;
}

void removePreds(ENode* enode, ENode_vec* enode_dag) {
    if (enode->CntrlPreds->size()) {
        for (auto& pred : *enode->CntrlPreds) {
            pred->CntrlSuccs->erase(
                std::remove(pred->CntrlSuccs->begin(), pred->CntrlSuccs->end(), enode),
                pred->CntrlSuccs->end());
            if (enode->type == Phi_c) {
                ENode* sinkNode = new ENode(Sink_, "sink");
                sinkNode->id    = enode_dag->size();
                sinkNode->bbId  = 0;
                enode_dag->push_back(sinkNode);
                pred->CntrlSuccs->push_back(sinkNode);
                sinkNode->CntrlPreds->push_back(pred);
            }
        }
        enode->CntrlPreds->clear();
    }
    if (enode->JustCntrlPreds->size()) {
        for (auto& pred : *enode->JustCntrlPreds) {
            pred->JustCntrlSuccs->erase(
                std::remove(pred->JustCntrlSuccs->begin(), pred->JustCntrlSuccs->end(), enode),
                pred->JustCntrlSuccs->end());
            if (enode->type == Phi_c) {
                ENode* sinkNode = new ENode(Sink_, "sink");
                sinkNode->id    = enode_dag->size();
                sinkNode->bbId  = 0;
                enode_dag->push_back(sinkNode);
                pred->JustCntrlSuccs->push_back(sinkNode);
                sinkNode->JustCntrlPreds->push_back(pred);
            }
        }
        enode->JustCntrlPreds->clear();
    }
}

void removeSuccs(ENode* enode, ENode_vec* enode_dag) {
    std::vector<ENode*> sinks;
    if (enode->CntrlSuccs->size()) {
        for (auto& pred : *enode->CntrlSuccs) {
            pred->CntrlPreds->erase(
                std::remove(pred->CntrlPreds->begin(), pred->CntrlPreds->end(), enode),
                pred->CntrlPreds->end());
            if (pred->type == Sink_)
                sinks.push_back(&*pred);
        }
        enode->CntrlSuccs->clear();
    }
    if (enode->JustCntrlSuccs->size()) {
        for (auto& pred : *enode->JustCntrlSuccs) {
            pred->JustCntrlPreds->erase(
                std::remove(pred->JustCntrlPreds->begin(), pred->JustCntrlPreds->end(), enode),
                pred->JustCntrlPreds->end());
            if (pred->type == Sink_)
                sinks.push_back(&*pred);
        }
        enode->JustCntrlSuccs->clear();
    }
    for (auto sink : sinks)
        eraseNode(sink, enode_dag);
}

void eraseNode(ENode* enode, ENode_vec* enode_dag) {
    assert(enode);
    removePreds(enode, enode_dag);
    removeSuccs(enode, enode_dag);
    enode_dag->erase(std::find(enode_dag->begin(), enode_dag->end(), enode));
}