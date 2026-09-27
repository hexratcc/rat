#include "ir/function.h"

#include "ir/module.h"

namespace rat {
	static_assert(AsmNode::controlProjIndex() == CallNode::controlProjIndex());
	static_assert(AsmNode::memoryProjIndex() == CallNode::memoryProjIndex());

	Module& Function::getModule() const { return *mod; }
	TypeContext& Function::types() const { return *mod; }
	const String& Function::getName() const { return name; }
	void Function::setName(String n) { name = std::move(n); }

	U32 Function::getParamCount() const { return (U32)paramTypes.size(); }
	Type* Function::getParamType(U32 index) const { return paramTypes[index]; }
	Type* Function::getReturnType() const { return retType; }
	B32 Function::returnsValue() const { return retType != nullptr; }

	const FunctionAttrs& Function::getAttrs() const { return attrs; }
	FunctionAttrs& Function::getAttrs() { return attrs; }

	StartNode* Function::getStart() const { return start; }
	StopNode* Function::getStop() const { return stop; }

	Type* Function::boolTy() const { return mod->getBool(); }
	Type* Function::ptrTy() const { return mod->getPtr(); }
	Type* Function::memTy() const { return mod->getMemory(); }
	Type* Function::ctrlTy() const { return mod->getControl(); }

	Function::Function(Module& module, String name, const List<Type*>& params, Type* ret)
	: mod(&module),
		name(std::move(name)),
		paramTypes(params),
		retType(ret) {
		// build the start tuple
		List<Type*> startElems{ctrlTy(), memTy()};
		startElems.insert(startElems.end(), paramTypes.begin(), paramTypes.end());

		start = create<StartNode>(mod->getTuple(startElems), getParamCount());
		stop = create<StopNode>(ctrlTy());
		paramCache.resize(paramTypes.size());

		memVar = newVar("mem", memTy());

		cur = createBlock();
		cur->ctrl = proj(start, StartNode::controlProjIndex(), ctrlTy(), "ctrl");
		cur->sealed = true;
		set(memVar, proj(start, StartNode::memoryProjIndex(), memTy(), "mem"));
	}

	Node* Function::control() const { return cur->ctrl; }
	B32 Function::blockFinished() const { return cur && cur->finished; }

	Node* Function::param(U32 index) {
		Node*& p = paramCache[index];
		if(!p) {
			String label = "arg" + std::to_string(index);
			p = proj(start, StartNode::paramProjIndex(index), paramTypes[index], std::move(label));
		}
		return p;
	}

	Node* Function::constInt(Type* type, I64 value) { return create<ConstantNode>(type, value); }
	Node* Function::constBool(B32 value) { return constInt(boolTy(), value ? 1 : 0); }
	Node* Function::constFloat(Type* type, F64 value) {
		if(type->getFloatWidth() == 32) {
			F32 f = (F32)value;
			U32 u;
			std::memcpy(&u, &f, sizeof(u));
			return constInt(type, (I64)(U64)u);
		}
		I64 bits;
		std::memcpy(&bits, &value, sizeof(bits));
		return constInt(type, bits);
	}

	Node* Function::binary(Opcode op, Node* lhs, Node* rhs) {
		return create<BinaryNode>(op, lhs->getType(), lhs, rhs);
	}
	Node* Function::add(Node* lhs, Node* rhs) { return binary(Opcode::Add, lhs, rhs); }
	Node* Function::sub(Node* lhs, Node* rhs) { return binary(Opcode::Sub, lhs, rhs); }
	Node* Function::mul(Node* lhs, Node* rhs) { return binary(Opcode::Mul, lhs, rhs); }
	Node* Function::sdiv(Node* lhs, Node* rhs) { return binary(Opcode::SDiv, lhs, rhs); }
	Node* Function::and_(Node* lhs, Node* rhs) { return binary(Opcode::And, lhs, rhs); }
	Node* Function::or_(Node* lhs, Node* rhs) { return binary(Opcode::Or, lhs, rhs); }
	Node* Function::shl(Node* lhs, Node* rhs) { return binary(Opcode::Shl, lhs, rhs); }
	Node* Function::lshr(Node* lhs, Node* rhs) { return binary(Opcode::LShr, lhs, rhs); }
	Node* Function::ashr(Node* lhs, Node* rhs) { return binary(Opcode::AShr, lhs, rhs); }

	Node* Function::unary(Opcode op, Node* in) { return create<UnaryNode>(op, in->getType(), in); }
	Node* Function::neg(Node* in) { return unary(Opcode::Neg, in); }
	Node* Function::bitNot(Node* in) { return unary(Opcode::Not, in); }
	Node* Function::ctz(Node* in) { return unary(Opcode::Ctz, in); }
	Node* Function::bswap(Node* in) { return unary(Opcode::Bswap, in); }

	Node* Function::compare(Opcode op, Node* lhs, Node* rhs) {
		return create<CompareNode>(op, boolTy(), lhs, rhs);
	}
	Node* Function::eq(Node* lhs, Node* rhs) { return compare(Opcode::Eq, lhs, rhs); }
	Node* Function::ne(Node* lhs, Node* rhs) { return compare(Opcode::Ne, lhs, rhs); }

	Node* Function::convert(Opcode op, Node* in, Type* to) { return create<ConvertNode>(op, to, in); }
	Node* Function::trunc(Node* in, Type* to) { return convert(Opcode::Trunc, in, to); }
	Node* Function::sext(Node* in, Type* to) { return convert(Opcode::SExt, in, to); }
	Node* Function::zext(Node* in, Type* to) { return convert(Opcode::ZExt, in, to); }

	Node* Function::load(Type* ty, Node* ptr) { return create<LoadNode>(ty, control(), mem(), ptr); }

	void Function::store(Node* pointer, Node* value) {
		set(memVar, create<StoreNode>(memTy(), control(), mem(), pointer, value));
	}

	Node* Function::global(const String& name) { return create<GlobalNode>(ptrTy(), name); }

	Node* Function::alloc(Type* type, U32 align) { return create<AllocNode>(ptrTy(), type, align); }

	Node* Function::stackAlloc(Node* byteCount) {
		return create<StackAllocNode>(ptrTy(), control(), mem(), byteCount);
	}

	Node* Function::stackSave() { return create<StackSaveNode>(ptrTy(), control(), mem()); }

	void Function::stackRestore(Node* saved) {
		set(memVar, create<StackRestoreNode>(memTy(), control(), mem(), saved));
	}

	void Function::bindEffects(Node* n) {
		Node* ctrlProj = proj(n, CallNode::controlProjIndex(), ctrlTy(), "ctrl");
		if(cur->ctrl)
			cur->ctrl = ctrlProj;
		set(memVar, proj(n, CallNode::memoryProjIndex(), memTy(), "mem"));
	}

	Node* Function::emitCall(const String& sym, const Nodes& ins, Type* ret, B32 indirect, B32 va) {
		List<Type*> elems{ctrlTy(), memTy()};
		if(ret)
			elems.push_back(ret);
		CallNode* c = create<CallNode>(mod->getTuple(elems), sym, ret != nullptr, ins, indirect);
		c->setVarArgs(va);
		bindEffects(c);
		if(ret)
			return proj(c, CallNode::valueProjIndex(), ret, "ret");
		return nullptr;
	}

	Node* Function::call(const String& callee, Type* retType, const Nodes& args, B32 varArgs) {
		List<Node*> ins{control(), mem()};
		ins.insert(ins.end(), args.begin(), args.end());
		return emitCall(callee, ins, retType, false, varArgs);
	}

	Node* Function::callIndirect(Node* target, Type* retType, const Nodes& args, B32 varArgs) {
		// inputs: control, memory, target pointer, then the call arguments
		List<Node*> ins{control(), mem(), target};
		ins.insert(ins.end(), args.begin(), args.end());
		return emitCall(String(), ins, retType, true, varArgs);
	}

	List<Node*> Function::inlineAsm(const String& text, const List<Type*>& outs, const Nodes& args) {
		List<Type*> elems{ctrlTy(), memTy()};
		elems.insert(elems.end(), outs.begin(), outs.end());
		List<Node*> ins{control(), mem()};
		ins.insert(ins.end(), args.begin(), args.end());

		AsmNode* a = create<AsmNode>(mod->getTuple(elems), text, outs.size(), ins);
		bindEffects(a);
		List<Node*> results;
		for(U32 i = 0; i < outs.size(); ++i)
			results.push_back(proj(a, AsmNode::outputProjIndex(i), outs[i], "out"));
		return results;
	}

	IfNode* Function::iff(Node* predicate) {
		return create<IfNode>(mod->getTuple({ctrlTy(), ctrlTy()}), control(), predicate);
	}
	ProjNode* Function::proj(Node* tuple, U32 index, Type* type, String label) {
		return create<ProjNode>(type, tuple, index, std::move(label));
	}
	PhiNode* Function::phi(Type* type, RegionNode* region, const Nodes& values) {
		List<Node*> ins{region};
		ins.insert(ins.end(), values.begin(), values.end());
		return create<PhiNode>(type, ins);
	}

	Function::Block* Function::createBlock(String) { return arena.make<Block>(); }

	Function::Block* Function::createLoopHeader(String) {
		Block* b = createBlock();
		b->region = create<RegionNode>(ctrlTy(), List<Node*>{});
		b->region->setLoopHeader();
		b->ctrl = b->region;
		return b;
	}

	void Function::addEdge(Node* exit, Block* to) {
		to->preds.push_back(exit);
		to->predBlocks.push_back(cur);
		if(to->region)
			to->region->addInput(exit);
	}

	void Function::activateOnSeal(Block* block) {
		if(block->ctrl)
			return; // loop headers and the entry are already active
		if(block->preds.size() >= 2) {
			block->region = create<RegionNode>(ctrlTy(), block->preds);
			block->ctrl = block->region;
		} else if(block->preds.size() == 1) {
			block->ctrl = block->preds[0]; // single predecessor needs no region
		}
		// zero predecessors: unreachable block; stays inactive
	}

	void Function::seal(Block* block) {
		activateOnSeal(block);
		for(auto& [var, phi] : block->incompletePhis)
			addPhiOperands(var, phi, block);
		block->incompletePhis.clear();
		block->sealed = true;
	}

	void Function::setInsertBlock(Block* block) { cur = block; }

	void Function::enterBlock(Block* block) {
		seal(block);
		setInsertBlock(block);
	}

	void Function::jmp(Block* target) {
		if(cur->ctrl)
			addEdge(cur->ctrl, target);
		cur->finished = true;
	}

	// multi-way jump: slot i of the range-checked selector goes to targets[i]
	void Function::switchJump(Node* selector, const List<Block*>& targets) {
		if(cur->ctrl) {
			List<Type*> elems(targets.size(), ctrlTy());
			SwitchNode* sw = create<SwitchNode>(mod->getTuple(elems), control(), selector);
			for(U32 i = 0; i < (U32)targets.size(); ++i)
				addEdge(proj(sw, i, ctrlTy(), "case"), targets[i]);
		}
		cur->finished = true;
	}

	void Function::jumpif(Node* cond, Block* target) {
		Node* elseP = nullptr;
		if(cur->ctrl) {
			IfNode* branch = iff(cond);
			Node* thenP = proj(branch, IfNode::thenProjIndex(), ctrlTy(), "then");
			elseP = proj(branch, IfNode::elseProjIndex(), ctrlTy(), "else");
			addEdge(thenP, target);
		}
		// the false path falls through into a fresh continuation block
		Block* fall = createBlock("ft");
		if(elseP)
			addEdge(elseP, fall);
		cur->finished = true;
		enterBlock(fall);
	}

	Function::Var Function::newVar(String, Type* type) {
		varTypes.push_back(type);
		return (Var)(varTypes.size() - 1);
	}

	Function::Var Function::declareLocal(String name, Node* init) {
		Var v = newVar(std::move(name), init->getType());
		set(v, init);
		return v;
	}

	Node* Function::get(Var var) { return read(var, cur); }
	void Function::set(Var var, Node* value) { cacheDef(cur, var, value); }
	Node* Function::mem() { return get(memVar); }

	Node** Function::findDef(Block* block, Var var) {
		for(U32 i = (U32)block->defs.size(); i > 0; --i)
			if(block->defs[i - 1].first == var)
				return &block->defs[i - 1].second;
		return nullptr;
	}

	void Function::cacheDef(Block* block, Var var, Node* val) {
		if(Node** slot = findDef(block, var))
			*slot = val;
		else
			block->defs.push_back({var, val});
		if(PhiNode* p = dyn_cast<PhiNode>(val))
			phiDefSites[p].push_back({block, var});
	}

	Node* Function::read(Var var, Block* block) {
		Node** slot = findDef(block, var);
		if(slot && *slot)
			return *slot;
		return readRecursive(var, block);
	}

	Node* Function::readRecursive(Var var, Block* block) {
		Node* val = nullptr;
		if(!block->sealed) {
			// unsealed (loop header): an incomplete phi, completed at seal()
			PhiNode* p = phi(varTypes[var], block->region, {});
			block->incompletePhis.push_back({var, p});
			val = p;
		} else if(block->preds.size() == 1) {
			val = read(var, block->predBlocks[0]);
		} else {
			PhiNode* p = phi(varTypes[var], block->region, {});
			cacheDef(block, var, p); // break cycles before reading predecessors
			val = addPhiOperands(var, p, block);
		}
		cacheDef(block, var, val);
		return val;
	}

	Node* Function::addPhiOperands(Var var, PhiNode* phi, Block* block) {
		for(Block* p : block->predBlocks)
			phi->addInput(read(var, p)); // aligns with region inputs by order
		return tryRemoveTrivialPhi(phi);
	}

	void Function::replacePhiEverywhere(PhiNode* phi, Node* with) {
		phi->replaceAllUsesWith(with);
		auto it = phiDefSites.find(phi);
		if(it == phiDefSites.end())
			return;
		List<Pair<Block*, Var>> sites = std::move(it->second);
		phiDefSites.erase(it);
		for(const auto& [block, var] : sites) {
			Node** slot = findDef(block, var);
			if(slot && *slot == phi)
				cacheDef(block, var, with);
		}
	}

	Node* Function::tryRemoveTrivialPhi(PhiNode* phi) {
		Node* same = nullptr;
		for(U32 i = 0, e = phi->getValueCount(); i < e; ++i) {
			Node* op = phi->getValue(i);
			if(op == phi || op == same)
				continue;
			if(same)
				return phi; // two distinct operands -> not trivial
			same = op;
		}
		if(!same)
			return phi; // no real operand yet; leave it

		List<PhiNode*> phiUsers;
		for(Node* u : phi->getUsers())
			if(u != phi && isa<PhiNode>(u))
				phiUsers.push_back(cast<PhiNode>(u));

		replacePhiEverywhere(phi, same);

		for(PhiNode* pu : phiUsers)
			tryRemoveTrivialPhi(pu);
		return same;
	}

	void Function::ret(Node* value) {
		cur->finished = true;
		if(!cur->ctrl)
			return; // unreachable block
		List<Node*> ins{control(), mem()};
		if(value)
			ins.push_back(value);
		stop->addInput(create<ReturnNode>(ctrlTy(), ins));
	}

	void Function::retVoid() { ret(nullptr); }

	B32 Function::hasReturn() const {
		for(Node* n : *this)
			if(isa<ReturnNode>(n))
				return true;
		return false;
	}

	B32 Function::isDeadNode(Node* n, B32 includeControl) const {
		B32 unused = !n->hasUsers() && !n->hasSideEffects() && (includeControl || !n->isCFG());
		B32 effect = isa<StoreNode>(n) || isa<CallNode>(n) || isa<AsmNode>(n);
		return (unused && n != start && n != stop) || (effect && !n->getControlInput());
	}

	U32 Function::eraseMarked(const List<U8>& mark) {
		U32 kept = 0;
		for(Node* n : nodes)
			if(!mark[n->getId()])
				nodes[kept++] = n;
		U32 removed = (U32)nodes.size() - kept;
		nodes.resize(kept);
		return removed;
	}

	U32 Function::eliminateDeadNodes(B32 includeControl) {
		List<U8> deadMark(nextId, 0); // id-indexed
		List<Node*> work(nodes.begin(), nodes.end());
		U32 count = 0;
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			if(deadMark[n->getId()] || !isDeadNode(n, includeControl))
				continue;
			deadMark[n->getId()] = 1;
			++count;
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				if(Node* in = n->getInput(i))
					work.push_back(in);
			n->clearInputs();
		}
		if(count) {
			touch();
			eraseMarked(deadMark);
		}
		return count;
	}

	U32 Function::pruneUnreachable() {
		Set<Node*> dead;
		List<Node*> work;
		for(Node* n : nodes) {
			B32 anchored = isa<LoadNode>(n) || isa<StoreNode>(n) || isa<CallNode>(n) || isa<AsmNode>(n) ||
										 isa<PhiNode>(n);
			if(anchored && !n->getControlInput()) {
				dead.insert(n);
				work.push_back(n);
			}
		}
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			for(Node* u : n->getUsers())
				if(dead.insert(u).second)
					work.push_back(u);
		}
		if(dead.empty())
			return 0;
		List<U8> deadMark(nextId, 0);
		for(Node* n : dead) {
			n->clearInputs();
			deadMark[n->getId()] = 1;
		}
		U32 removed = eraseMarked(deadMark);
		if(removed)
			touch();
		return removed;
	}

	void Function::removeNode(Node* n) {
		touch();
		n->clearInputs();
		auto it = std::find(nodes.begin(), nodes.end(), n);
		if(it != nodes.end())
			nodes.erase(it);
	}

	U32 Function::allocateId() {
		touch();
		return nextId++;
	}
} // namespace rat
