#include "ElasticPass/CircuitGenerator.h"
#include "ElasticPass/Utils.h"
#include "ElasticPass/Memory.h"
#include "MemElemInfo/MemUtils.h"
#include "OptimizeBitwidth/OptimizeBitwidth.h"

/**
 * @brief connectEnodeWithMCEnode
 * Performs the necessary connections between a memory instruction
 * enode and a memory controller/LSQ enode once it has been determined
 * that the two should connect.
 */
void connectEnodeWithMCEnode(ENode* enode, ENode* mcEnode, BBNode* bb) {
    enode->Mem = mcEnode;
    enode->CntrlSuccs->push_back(mcEnode);
    mcEnode->CntrlPreds->push_back(enode);
    enode->memPort = true;
    setBBIndex(enode, bb);
    setMemPortIndex(enode, mcEnode);
    setBBOffset(enode, mcEnode);
}

/**
 * @brief getOrCreateMCEnode
 * @param base: base address of the array associated with the MC
 * @returns pointer to MC Enode associate with the @p base
 */
ENode* CircuitGenerator::getOrCreateMCEnode(const Value* base,
                                            std::map<const Value*, ENode*>& baseToMCEnode) {
    auto iter     = baseToMCEnode.find(base);
    auto* mcEnode = iter->second;
    if (iter == baseToMCEnode.end()) {
        mcEnode             = new ENode(MC_, base->getName().str().c_str());
        baseToMCEnode[base] = mcEnode;
        // Add new MC to enode_dag
        enode_dag->push_back(mcEnode);
    }
    return mcEnode;
}

/**
 * @brief CircuitGenerator::addMCForEnode
 * For a given load or store instruction (which has not previously
 * been connected to an LSQ), associate it with an MC.
 * @param enode
 * @param baseToMCEnode
 */
void CircuitGenerator::addMCForEnode(ENode* enode, std::map<const Value*, ENode*>& baseToMCEnode) {
    Instruction* I = enode->Instr;
    auto* mcEnode  = getOrCreateMCEnode(findBase(I), baseToMCEnode);
    connectEnodeWithMCEnode(enode, mcEnode, findBB(enode));
    updateMCConstant(enode, mcEnode);
}

void CircuitGenerator::constructLSQNodes(std::map<const Value*, ENode*>& LSQnodes) {
    /* Create an LSQ enode for each of the LSQs reported
     * by MemElemInfo, and create a mapping between the unique
     * bases of the LSQs and the corresponding LSQ enodes */
    auto LSQlist = MEI.getLSQList();
    for (auto lsq : LSQlist) {
        auto* base     = lsq->base;
        auto* e        = new ENode(LSQ_, base->getName().str().c_str());
        LSQnodes[base] = e;
    }

    /* Append all the LSQ ENodes to the enode_dag */
    for (const auto& it : LSQnodes)
        enode_dag->push_back(it.second);
}

void CircuitGenerator::updateMCConstant(ENode* enode, ENode* mcEnode) {
    /* Each memory controller contains a notion of how many
     * store operations will be executed for a given BB, when
     * entering said BB. This constant is used to ensure that
     * all pending stores are completed, before the program is
     * allowed to terminate.
     * If present, locate this constant node and increment the
     * count, else create a new constant node.
     */

    if (enode->Instr->getOpcode() == Instruction::Store) {
        bool found = false;
        for (auto& pred : *mcEnode->JustCntrlPreds)
            if (pred->BB == enode->BB && pred->type == Cst_) {
                pred->cstValue++;
                found = true;
             }
        if (!found) {
            ENode* cstNode    = new ENode(Cst_, enode->BB);
            cstNode->cstValue = 1;
            cstNode->JustCntrlSuccs->push_back(mcEnode);
            cstNode->id = cst_id++;
            mcEnode->JustCntrlPreds->push_back(cstNode);

            // Add new constant to enode_dag
            enode_dag->push_back(cstNode);
        }
    }
}

// find specific node type in a given basic block
ENode* CircuitGenerator::findNodeInBB(node_type type, BBNode* bb)
{
    for (unsigned int i = 0; i < enode_dag->size(); i++)
    {
        auto* enode = enode_dag->at(i);
        if (findBB(enode) == bb && enode->type == type)
        {
            return enode;
        }
    }
    // TODO: only one control branch each bb? need to double check
    return nullptr;
}

ENode* CircuitGenerator::getDataInput(ENode* node)
{
    for (auto& inputNode : *node->JustCntrlPreds)
    {
        if (inputNode->type == Fork_c)
        {
            ENode* predInputNode = inputNode->JustCntrlPreds->at(0);
            if (predInputNode->type == Branch_)
            {
                continue;
            }
        } else if (inputNode->type == Fork_)
        {
            ENode* predInputNode = inputNode->CntrlPreds->at(0);
            if (predInputNode->type == Branch_)
            {
                continue;
            }
        }

        if (inputNode->type == Branch_)
        {
            continue;
        }
        return inputNode;
    }
    return nullptr;
}

void CircuitGenerator::insertBetween(ENode* nodePreds, ENode* nodeSuccs, ENode* insertNode)
{
    for (unsigned int i = 0; i < nodePreds->JustCntrlSuccs->size(); i++)
    {
        if (nodePreds->JustCntrlSuccs->at(i) == nodeSuccs)
        {
            nodePreds->JustCntrlSuccs->at(i) = insertNode;
            insertNode->JustCntrlPreds->push_back(nodePreds);
            break;
        }
    }
    for (unsigned int i = 0; i < nodeSuccs->JustCntrlPreds->size(); i++)
    {
        if (nodeSuccs->JustCntrlPreds->at(i) == nodePreds)
        {
            nodeSuccs->JustCntrlPreds->at(i) = insertNode;
            insertNode->JustCntrlSuccs->push_back(nodeSuccs);
            break;
        }
    }
}

void CircuitGenerator::setId(ENode* node)
{
    for (unsigned int i = enode_dag->size() - 1; i > 0; i--)
    {
        ENode* enode = enode_dag->at(i);
        if (enode->type == node->type)
        {
            node->id = enode->id + 1;
            break;
        }
    }
    if (node->id == -1)
    {
        node->id = 0;
    }
}

// check if there is already a memory acknowledgement signal for this load/store instr
// if no, insert a fork for later use;
// if yes, reuse the previous fork.
void CircuitGenerator::mergeOrInsertMemoryAck(ENode* InstNode, ENode* memNode, ENode* succ)
{
    int index = InstNode->MemoryAckIndex;
    if (index == -1)
    {
        ENode* forkC_node = new ENode(Fork_c, "forkC", InstNode->BB);
        setId(forkC_node);

        memNode->JustCntrlSuccs->push_back(forkC_node);
        forkC_node->JustCntrlPreds->push_back(memNode);
        forkC_node->JustCntrlSuccs->push_back(succ);
        succ->JustCntrlPreds->push_back(forkC_node);

        enode_dag->push_back(forkC_node);
        InstNode->MemoryAckIndex = memNode->JustCntrlSuccs->size() - 1;
    }
    else
    {
        ENode* forkC_node = memNode->JustCntrlSuccs->at(index);
        forkC_node->JustCntrlSuccs->push_back(succ);
        succ->JustCntrlPreds->push_back(forkC_node);
    }
}

// create another Join that joins the control input
// to trigger Load/Store operations
void CircuitGenerator::setLoadOrStoreTriggerInputs(ENode* memNode, std::vector<ENode*>& memoryInst)
{
    for (unsigned int i = 0; i < memoryInst.size(); i++)
    {
        // stores control inputs that triggers the store/load
        std::vector<ENode*> control_in_trigger;
        ENode* currNode = memoryInst[i];
        BBNode* currentBB = findBB(currNode);
        if (currNode->BB->getParent() == nullptr)
        {
            control_in_trigger.push_back(findNodeInBB(Start_, currentBB));
        }
        else if(findNodeInBB(Phi_c, currentBB) != nullptr) // if there is a control Phi node (work as control merge)
        {
            ENode* phic = findNodeInBB(Phi_c, currentBB);
            if (phic->JustCntrlSuccs->at(0)->type == Fork_c)
            {
                control_in_trigger.push_back(phic->JustCntrlSuccs->at(0));
            }
            else
            {
                control_in_trigger.push_back(phic);
            }
        }
        
        // check previous load/store
        for (unsigned int j = 0; j < i; ++j)
        {
            ENode* prevNode = memoryInst[j];
            BBNode* prevBB = findBB(prevNode);
            if (currentBB == prevBB
                && !(isa<LoadInst>(currNode->Instr)
                && isa<LoadInst>(prevNode->Instr)))
            {
                control_in_trigger.push_back(memNode);
            }
        }

        std::vector<ENode*> hasJoin; // collects nodes that has already been connected to Join
        
        // if there is only one control trigger, connect directly to load/store
        if (control_in_trigger.size() == 1)
        {
            currNode->JustCntrlPreds->push_back(control_in_trigger[0]);
            control_in_trigger[0]->JustCntrlSuccs->push_back(currNode);
        }
        else // if more than one, add a JOIN to join then and connect
        {
            ENode* JoinEnode = new ENode(Join_, "join", currentBB->BB);
            setId(JoinEnode);
            for (auto& triggerNode : control_in_trigger)
            {
                if (triggerNode->type == MC_)
                {
                    mergeOrInsertMemoryAck(currNode, memNode, JoinEnode);
                }
                else if (std::find(hasJoin.begin(), hasJoin.end(), triggerNode) == hasJoin.end())
                {
                    hasJoin.push_back(triggerNode);
                    triggerNode->JustCntrlSuccs->push_back(JoinEnode);
                    JoinEnode->JustCntrlPreds->push_back(triggerNode);
                }
            }
            enode_dag->push_back(JoinEnode);
            JoinEnode->JustCntrlSuccs->push_back(currNode);
            currNode->JustCntrlPreds->push_back(JoinEnode);
        }
    }
}



// check if there is an exist memory node to associate with
// if not, create a new memory node and associate it with the instruction node parameter
void CircuitGenerator::addMemoryNode(ENode* enode, std::map<const Value*, ENode*>& MemNodesCollect, std::map<ENode*, std::vector<ENode*>>& MemInstCollect)
{
    Instruction* I = enode->Instr;
    const Value* base = findBase(I);
    auto iter = MemNodesCollect.find(base);
    ENode* memoryNode = iter->second;
    if (iter == MemNodesCollect.end())
    {
        memoryNode = new ENode(MC_, base->getName().str().c_str());
        setId(memoryNode);
        MemNodesCollect[base] = memoryNode;
        enode_dag->push_back(memoryNode);
        std::vector<ENode*> InstCollect;
        MemInstCollect[memoryNode] = InstCollect;
    }
    MemInstCollect[memoryNode].push_back(enode);

    connectEnodeWithMCEnode(enode, memoryNode, findBB(enode));
    if (enode->Instr->getOpcode() == Instruction::Load)
    {
        memoryNode->CntrlSuccs->push_back(enode);
        enode->CntrlPreds->push_back(memoryNode);
    }
    if (enode->Instr->getOpcode() == Instruction::Store)
    {
        memoryNode->CntrlPreds->push_back(enode);
        enode->CntrlSuccs->push_back(memoryNode);
    }
}

// called only if use Memory Node
void CircuitGenerator::addMemoryNodesConnection()
{
    // // contains load/store nodes corresponding to their base addresses
    // std::map<const Value*, std::vector<ENode*>> LSNodesCollect;
    // // if there is no memory dependency we don't need a SMC
    // bool need_SMC = 0;

    // for (unsigned int i = 0; i < enode_dag->size(); i++) {
    //     ENode* enode = enode_dag->at(i);
    //     if (enode->isLoadOrStore())
    //     {
    //         const Value* base = findBase(enode->Instr);
    //         auto iter = LSNodesCollect.find(base);
    //         if (iter == LSNodesCollect.end())
    //         {
    //             std::vector<ENode*> InstCollect;
    //             InstCollect.push_back(enode);
    //             LSNodesCollect[base] = InstCollect;
    //         } else // more than one instructions
    //         {
    //             if (enode->Instr->getOpcode() == Instruction::Store)
    //             {
    //                 need_SMC = 1;
    //                 break;
    //             } else
    //             {
    //                 for (int i = 0; i < iter->second.size(); i++)
    //                 {
    //                     ENode* prevNode = iter->second.at(i);
    //                     if (prevNode->Instr->getOpcode() == Instruction::Store)
    //                     {
    //                         need_SMC = 1;
    //                         break;
    //                     } else
    //                     {
    //                         LSNodesCollect[base].push_back(enode);
    //                     }
    //                 }
    //             }
    //         }
    //     }
    // }

    // if (!need_SMC)
    // {
    //     addMemoryInterfaces(false);
    //     return;
    // }

    // Contains memory interface ENodes for each array, corresponding to its base address
    std::map<const Value*, ENode*> MemNodesCollect;

    // Contains all memory instruction (load/store) for each array, corresponding to their memory interface node
    std::map<ENode*, std::vector<ENode*>> MemInstCollect;

    // Classify Load/Store nodes and connect with corresponding MemNode
    // for (unsigned int i = 0; i < enode_dag->size(); i++) {
    //     ENode* enode = enode_dag->at(i);
    //     if (!enode->isLoadOrStore())
    //         continue;
    //     // In case it's a Load or Store, find its associated memory node
    //     if (enode->isLoadOrStore())
    //     {
    //         addMemoryNode(enode, MemNodesCollect, MemInstCollect);
    //     }
    // }

    ENode* BranchcNode = nullptr;
    ENode* PhicNode = nullptr;
    ENode* forkNode = nullptr;
    ENode* joinNode = nullptr;
    ENode* storeAck = nullptr;
    ENode* storeNode = nullptr;
    ENode* loadNode = nullptr;

    for (unsigned int i = 0; i < enode_dag->size(); i++) {
        ENode* enode = enode_dag->at(i);
        if (enode->isLoadOrStore())
        {
            if (isa<LoadInst>(enode->Instr))
            {
                loadNode = enode;
                addMemoryNode(enode, MemNodesCollect, MemInstCollect);
            }
        }
    }

    for (unsigned int i = 0; i < enode_dag->size(); i++) {
        ENode* enode = enode_dag->at(i);
        if (enode->isLoadOrStore())
        {
            if (isa<StoreInst>(enode->Instr))
            {
                storeNode = enode;
                addMemoryNode(enode, MemNodesCollect, MemInstCollect);
            }
        }
    }

    // iterate through all load/store nodes connect to each MemoryNode
    for (auto mem : MemInstCollect)
    {
       // connect to end node
       // Create a synchronisation join for each basic block between the control merge and the control branch
       for (auto& InstNode : mem.second)
       {
           BBNode* bb = findBB(InstNode); // for each block touches the array
           ENode* controlBranch = findNodeInBB(Branch_c, bb);
           ENode* dataInput = getDataInput(controlBranch);
           // add a Join node
           if (dataInput == nullptr || dataInput->type == Join_)
           {
               mergeOrInsertMemoryAck(InstNode, mem.first, dataInput);
               continue;
           }
           ENode* JoinEnode = new ENode(Join_, "join", controlBranch->BB);
           setId(JoinEnode);

           insertBetween(dataInput, controlBranch, JoinEnode);
           enode_dag->push_back(JoinEnode);

           // connect Memory's acknowledgement output to JOIN
           mergeOrInsertMemoryAck(InstNode, mem.first, JoinEnode);

           BranchcNode = controlBranch;
       }
       // Step 4: Create a new JOIN to join inputs that trigger Load/Store
       setLoadOrStoreTriggerInputs(mem.first, mem.second);

    }

    bool insert_fifo = 0;
    ENode* load_addrPred = nullptr;
    ENode* store_addrPred = nullptr;

    int load_cstIndex = 0;
    int store_cstIndex = 0;
    int dep_distance = -1;

    if (loadNode != nullptr && storeNode != nullptr)
    {
        PhicNode = BranchcNode->JustCntrlSuccs->at(0);
        if (PhicNode->type == Phi_c)
        {
            insert_fifo = 1;
            load_addrPred = loadNode->CntrlPreds->at(0);
            store_addrPred = storeNode->CntrlPreds->at(1);
        }
    }

    while (insert_fifo)
    {
        std::cerr << "load_addrPred: " << load_addrPred->Name << std::endl;
        std::cerr << "load_addrPred cntrl pred: " << load_addrPred->CntrlPreds->at(0)->type << std::endl;
        if (load_addrPred->type == Fork_ && 
            load_addrPred->CntrlPreds->at(0)->type == Phi_)
        {
            std::cerr << "load exit 1" << std::endl;
            break;
        } else if (load_addrPred->type == Fork_ && 
            load_addrPred->CntrlPreds->at(0)->Name == "add")
        {
            load_addrPred = load_addrPred->CntrlPreds->at(0);
        }

        if (load_addrPred->Name == "add")
        {
            std::cerr << "find an add with load!" << std::endl;
            if (load_addrPred->CntrlPreds->at(0)->type == Cst_)
            {
                load_cstIndex += stoi(load_addrPred->CntrlPreds->at(0)->Name);
                load_addrPred = load_addrPred->CntrlPreds->at(0);
            } else
            {
                insert_fifo = 0;
            }
        } else
        {
            insert_fifo = 0;
        }
    }

    while (insert_fifo)
    {
        std::cerr << "store_addrPred: " << store_addrPred->Name << std::endl;
        if (store_addrPred->type == Fork_ && 
            store_addrPred->CntrlPreds->at(0)->type == Phi_n)
        {
            std::cerr << "store exit 1" << std::endl;
            break;
        } else if (store_addrPred->type == Fork_ && 
            store_addrPred->CntrlPreds->at(0)->Name == "add")
        {
            store_addrPred = store_addrPred->CntrlPreds->at(0);
        }

        if (store_addrPred->Name == "add")
        {
            // std::cerr << "find an add with store!" << std::endl;
            if (store_addrPred->CntrlPreds->at(0)->type == Cst_)
            {
                store_cstIndex += stoi(store_addrPred->CntrlPreds->at(0)->Name);
                store_addrPred = store_addrPred->CntrlPreds->at(1);
            } else
            {
                insert_fifo = 0;
            }
        } else
        {
            insert_fifo = 0;
        }
    }

    std::cerr << "insert fifo: " << insert_fifo << std::endl;

    dep_distance = store_cstIndex - load_cstIndex;
    std::cerr << "Branch node: " << BranchcNode->Name << BranchcNode->id << std::endl;
    dep_distance = 2;
    insert_fifo = 1;

    std::cerr << "storeCst value: " << store_cstIndex << std::endl;
    std::cerr << "loadCst value: " << load_cstIndex << std::endl;
    std::cerr << "dep distance: " << dep_distance << std::endl;

    insert_fifo = 0; //switch, force it not to add smc fifo.

    ENode* endNode = nullptr;

    if (insert_fifo && dep_distance > 1)
    {
        std::cerr << "enter!" << std::endl;
        ENode* smcFifo = new ENode(Fifosmc_, "smcFifo", BranchcNode->BB);
        smcFifo->smcFifoDepth = dep_distance - 1;
        setId(smcFifo);
        enode_dag->push_back(smcFifo);

        joinNode = BranchcNode->JustCntrlPreds->at(0);
        insertBetween(joinNode, BranchcNode, smcFifo);

        BranchcNode = BranchcNode->JustCntrlSuccs->at(0);
        BranchcNode = BranchcNode->JustCntrlSuccs->at(0);
        if (BranchcNode->type == Fork_c)
        {
            BranchcNode = BranchcNode->JustCntrlSuccs->at(0);
        }
        std::cerr << "Branch node now: " << BranchcNode->Name << BranchcNode->id << std::endl;

        endNode = BranchcNode->JustCntrlSuccs->at(1);
        // endNode = BranchcNode->JustCntrlSuccs->at(0);
        ENode* smcCntrl = new ENode(Cntrlsmc_, "smcCntrl", BranchcNode->BB);
        smcCntrl->smcFifoDepth = dep_distance - 1;
        setId(smcCntrl);
        enode_dag->push_back(smcCntrl);
        insertBetween(BranchcNode, endNode, smcCntrl);

    }
}


/**
 * @brief CircuitGenerator::addMemoryInterfaces
 * This pass is responsible for emitting all connections between
 * memory accessing instructions and associated memory controllers.
 * If @p useLSQ, LSQs will be emitted for all instructions determined
 * to have possible data dependencies by the MemElemInfo pass.
 */
void CircuitGenerator::addMemoryInterfaces(const bool useLSQ) {
    std::map<const Value*, ENode*> LSQnodes;

    if (useLSQ) {
        constructLSQNodes(LSQnodes);
    }

    // for convenience, we store a mapping between the base arrays in a program,
    // and their associated memory interface ENode (MC or @todo LSQ)
    std::map<const Value*, ENode*> baseToMCEnode;

    /* For each memory instruction present in the enode_dag, connect it
     * to a previously created LSQ if MEI has determined so, else, connect
     * it to a regular MC.
     * Iterate using index, given that iterators are invalidated once a
     * container is modified (which happens during addMCForEnode).
     */
    for (unsigned int i = 0; i < enode_dag->size(); i++) {
        auto* enode = enode_dag->at(i);
        if (!enode->isLoadOrStore())
            continue;

        if (useLSQ && MEI.needsLSQ(enode->Instr)) {
            const LSQset& lsq = MEI.getInstLSQ(enode->Instr);
            connectEnodeWithMCEnode(enode, LSQnodes[lsq.base], findBB(enode));
            continue;
        }

        addMCForEnode(enode, baseToMCEnode);
    }

    // Check if LSQ and MC are targetting the same memory
    for (auto lsq : LSQnodes) {
        auto* base     = lsq.first;
        if (baseToMCEnode[base] != NULL) {
            auto *lsqNode = lsq.second;
            lsqNode->lsqToMC = true;
            auto *mcEnode = baseToMCEnode[base];
            mcEnode->lsqToMC = true;

            // LSQ will connect as a predecessor of MC
            // MC should keep track of stores arriving from LSQ
            for (auto& enode : *lsqNode ->CntrlPreds) 
               updateMCConstant(enode, mcEnode);
            
            lsqNode->CntrlSuccs->push_back(mcEnode);
            mcEnode->CntrlPreds->push_back(lsqNode);
            //setBBIndex(lsqNode, mcEnode->bbNode);
            setLSQMemPortIndex(lsqNode, mcEnode);
            setBBOffset(lsqNode, mcEnode);
        }
    }
}

// set index of BasicBlock in load/store connected to MC or LSQ
void setBBIndex(ENode* enode, BBNode* bbnode) { enode->bbId = bbnode->Idx + 1; }

// load/store port index for LSQ-MC connection
void setLSQMemPortIndex(ENode* enode, ENode* memnode) {

    assert (enode->type == LSQ_);
    int loads = 0;
    int stores = 0;
    for (auto& node : *memnode->CntrlPreds) {
	    if (enode == node)
	        break;
	    if (node->type == LSQ_){
	        loads++;
	        stores++;
	    }
	    else {
	        auto* I = node->Instr;
	        if (isa<LoadInst>(I))
	        	loads++;
	        else {
	        	assert (isa<StoreInst>(I));
	        	stores++;
	        }
	    }
	}
	enode->lsqMCLoadId = loads; 
    enode->lsqMCStoreId = stores;
}

// load/store port index for LSQ or MC
void setMemPortIndex(ENode* enode, ENode* memnode) {
    int offset = 0;
    for (auto& node : *memnode->CntrlPreds) {
        if (enode == node)
            break;
        if (!compareMemInst(enode, node))
            offset++;
    }
    enode->memPortId = offset;

    if (isa<StoreInst>(enode->Instr))
    {
        enode->memPortId /= 2;
    }

    if (enode->type == LSQ_){
    	int loads = 0;
    	int stores = 0;
    	for (auto& node : *memnode->CntrlPreds) {
	        if (enode == node)
	            break;
	        if (node->type == LSQ_){
	            loads++;
	            stores++;
	        }
	        else {
	        	auto* I = node->Instr;
	        	if (isa<LoadInst>(I))
	        		loads++;
	        	else {
	        		assert (isa<StoreInst>(I));
	        		stores++;
	        	}
	        }
	    }
	    enode->lsqMCLoadId = loads; 
        enode->lsqMCStoreId = stores;
	}
}

// ld/st offset within bb (for LSQ or MC grop allocator)
void setBBOffset(ENode* enode, ENode* memnode) {
    int offset = 0;
    for (auto& node : *memnode->CntrlPreds) {
        if (enode == node)
            break;
        if (compareMemInst(enode, node) && enode->BB == node->BB)
            offset++;
    }
    enode->bbOffset = offset;
}

int getBBIndex(ENode* enode) { return enode->bbId; }

int getMemPortIndex(ENode* enode) { return enode->memPortId; }

int getBBOffset(ENode* enode) { return enode->bbOffset; }

// check if two mem instructions are different type (for offset calculation)
bool compareMemInst(ENode* enode1, ENode* enode2) {
	// When MC connected to LSQ
	// LSQ has both load and store port connected to MC 
	// Need to increase offset of both load and store
    if (enode1->type == LSQ_ || enode2->type == LSQ_)
        return false;

    auto* I1 = enode1->Instr;
    auto* I2 = enode2->Instr;

    return ((isa<LoadInst>(I1) && isa<StoreInst>(I2)) || (isa<LoadInst>(I2) && isa<StoreInst>(I1)));
}

// total count of loads connected to lsq
int getMemLoadCount(ENode* memnode) {
    int ld_count = 0;
    for (auto& enode : *memnode->CntrlPreds) {
        if (enode->type != Cst_ && enode->type != LSQ_) {
            auto* I = enode->Instr;
            if (isa<LoadInst>(I))
                ld_count++;
        }
    }
    return ld_count;
}

// total count of stores connected to lsd
int getMemStoreCount(ENode* memnode) {
    int st_count = 0;
    for (auto& enode : *memnode->CntrlPreds) {
        if (enode->type != Cst_ && enode->type != LSQ_) {
            auto* I = enode->Instr;
            if (isa<StoreInst>(I))
                st_count++;
        }
    }
    // if (st_count == 0)
    // {
    //     st_count = -1;
    // } else
    // {
    //     st_count = st_count / 2;
    // }

    st_count = st_count / 2;
    
    return st_count;
}

// total count of bbs connected to lsq (corresponds to control fork connection
// count)
int getMemBBCount(ENode* memnode) { return memnode->JustCntrlPreds->size(); }

// total input count to MC/LSQ
int getMemInputCount(ENode* memnode) {
    return getMemBBCount(memnode) + 2 * getMemStoreCount(memnode) + getMemLoadCount(memnode);
}

// total output count to MC/LSQ
int getMemOutputCount(ENode* memnode) { return getMemLoadCount(memnode); }

// adress index for dot (fork connections go first)
int getDotAdrIndex(ENode* enode, ENode* memnode) {
    int count = 0;
    for (auto& node : *memnode->CntrlPreds) {
        if (node == enode)
            break;
        auto* I = node->Instr;
        if (isa<StoreInst>(I))
            // count += 2;
            count += 1;
        else
            count++;
    }

    return getMemBBCount(memnode) + count + 1;
}

// data index for dot
int getDotDataIndex(ENode* enode, ENode* memnode) {
    auto* I = enode->Instr;
    if (isa<StoreInst>(I)) {
        return getDotAdrIndex(enode, memnode) + 1;
    } else {
        int count = 0;

        for (auto& node : *memnode->CntrlPreds) {
            if (node == enode)
                break;
            auto* I = node->Instr;
            if (isa<LoadInst>(I))
                count++;
        }
        return count + 1;
    }
}

int getBBLdCount(ENode* enode, ENode* memnode) {
    int ld_count = 0;

    for (auto& node : *memnode->CntrlPreds) {

        if (node->type != Cst_) {
            auto* I = node->Instr;
            if (isa<LoadInst>(I) && node->BB == enode->BB)
                ld_count++;
        }
    }

    return ld_count;
}

int getBBStCount(ENode* enode, ENode* memnode) {
    int st_count = 0;

    for (auto& node : *memnode->CntrlPreds) {

        if (node->type != Cst_) {
            auto* I = node->Instr;
            if (isa<StoreInst>(I) && node->BB == enode->BB)
                st_count++;
        }
    }

    return st_count;
}

int getLSQDepth(ENode* memnode) {
    // Temporary: determine LSQ depth based on the BB with largest ld/st count
    // Does not guarantee maximal throughput!

    int d = 0;

    for (auto& enode : *memnode->JustCntrlPreds) {
        // BBNode* bb = enode->bbNode;

        int ld_count = getBBLdCount(enode, memnode);
        int st_count = getBBStCount(enode, memnode);

        d = (d < ld_count) ? ld_count : d;
        d = (d < st_count) ? st_count : d;
    }

    int power = 16;

    // round up to power of two
    while (power < d)
        power *= 2;

    // increase size for performance (not optimal!)
    // power *= 2;

    return power;
}
