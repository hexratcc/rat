#include "IR/Function.h"

#include "CodeGen/Target.h"
#include "IR/Module.h"

#include <unordered_map>

namespace rat {
	struct Function::Block {
		String name;
		RegionNode* region = nullptr;
		List<Node*> preds;
		List<Block*> predBlocks; // source block per pred (parallel)
		Node* ctrl = nullptr; // control anchor once active
		B32 sealed = false;
		B32 active = false; // ctrl established
		B32 loopHeader = false;
		B32 finished = false; // ended in a terminator
		Map<U32, Node*> defs;
		Map<U32, PhiNode*> incompletePhis;
	};

	Module& Function::getModule() const { return *mod; }
	TypeContext& Function::types() const { return *mod; }
	const String& Function::getName() const { return name; }

	U32 Function::getParamCount() const { return (U32)paramTypes.size(); }
	Type* Function::getParamType(U32 index) const { return paramTypes[index]; }
	Type* Function::getReturnType() const { return retType; }
	B32 Function::returnsValue() const { return retType != nullptr; }

	StartNode* Function::getStart() const { return start; }
	StopNode* Function::getStop() const { return stop; }

	Type* Function::boolTy() const { return mod->getBool(); }
	Type* Function::intTy(U32 bits) const { return mod->getInt(bits); }
	Type* Function::ptrTy() const { return mod->getPtr(); }
	Type* Function::memTy() const { return mod->getMemory(); }
	Type* Function::ctrlTy() const { return mod->getControl(); }

	Function::Function(Module& module, String name, const List<Type*>& params,
										 Type* ret)
			: mod(&module), name(std::move(name)), paramTypes(params), retType(ret) {
		TypeContext& tc = *mod;

		// build the start tuple
		List<Type*> startElems;
		startElems.push_back(tc.getControl());
		startElems.push_back(tc.getMemory());
		for (Type* p : paramTypes)
			startElems.push_back(p);

		start = create<StartNode>(tc.getTuple(startElems), getParamCount());
		stop = create<StopNode>(tc.getControl());

		memVar = newVar("mem", memTy());

		Block* entry = makeBlock("entry", false);
		entry->ctrl = proj(start, StartNode::controlProjIndex(), ctrlTy(), "ctrl");
		entry->active = true;
		entry->sealed = true;
		cur = entry;

		writeVar(memVar, proj(start, StartNode::memoryProjIndex(), memTy(), "mem"));
	}

	Function::~Function() = default;

	Node* Function::control() const { return cur->ctrl; }
	Node* Function::memory() { return readVar(memVar); }
	Function::Block* Function::insertBlock() const { return cur; }
	B32 Function::blockFinished() const { return cur && cur->finished; }

	Node* Function::param(U32 index) {
		if (paramCache.size() <= index)
			paramCache.resize(index + 1, nullptr);
		if (!paramCache[index])
			paramCache[index] =
					proj(start, StartNode::paramProjIndex(index), paramTypes[index],
							 "arg" + std::to_string(index));
		return paramCache[index];
	}

	Node* Function::constInt(Type* type, I64 value) {
		return create<ConstantNode>(type, value);
	}
	Node* Function::constBool(B32 value) {
		return constInt(boolTy(), value ? 1 : 0);
	}

	Node* Function::binary(Opcode op, Node* lhs, Node* rhs) {
		return create<BinaryNode>(op, lhs->getType(), lhs, rhs);
	}
	Node* Function::add(Node* lhs, Node* rhs) {
		return binary(Opcode::Add, lhs, rhs);
	}
	Node* Function::sub(Node* lhs, Node* rhs) {
		return binary(Opcode::Sub, lhs, rhs);
	}
	Node* Function::mul(Node* lhs, Node* rhs) {
		return binary(Opcode::Mul, lhs, rhs);
	}
	Node* Function::sdiv(Node* lhs, Node* rhs) {
		return binary(Opcode::SDiv, lhs, rhs);
	}
	Node* Function::udiv(Node* lhs, Node* rhs) {
		return binary(Opcode::UDiv, lhs, rhs);
	}
	Node* Function::srem(Node* lhs, Node* rhs) {
		return binary(Opcode::SRem, lhs, rhs);
	}
	Node* Function::urem(Node* lhs, Node* rhs) {
		return binary(Opcode::URem, lhs, rhs);
	}
	Node* Function::and_(Node* lhs, Node* rhs) {
		return binary(Opcode::And, lhs, rhs);
	}
	Node* Function::or_(Node* lhs, Node* rhs) {
		return binary(Opcode::Or, lhs, rhs);
	}
	Node* Function::xor_(Node* lhs, Node* rhs) {
		return binary(Opcode::Xor, lhs, rhs);
	}
	Node* Function::shl(Node* lhs, Node* rhs) {
		return binary(Opcode::Shl, lhs, rhs);
	}
	Node* Function::lshr(Node* lhs, Node* rhs) {
		return binary(Opcode::LShr, lhs, rhs);
	}
	Node* Function::ashr(Node* lhs, Node* rhs) {
		return binary(Opcode::AShr, lhs, rhs);
	}

	Node* Function::unary(Opcode op, Node* operand) {
		return create<UnaryNode>(op, operand->getType(), operand);
	}
	Node* Function::neg(Node* operand) { return unary(Opcode::Neg, operand); }
	Node* Function::bitNot(Node* operand) { return unary(Opcode::Not, operand); }

	Node* Function::compare(Opcode op, Node* lhs, Node* rhs) {
		return create<CompareNode>(op, boolTy(), lhs, rhs);
	}
	Node* Function::eq(Node* lhs, Node* rhs) {
		return compare(Opcode::Eq, lhs, rhs);
	}
	Node* Function::ne(Node* lhs, Node* rhs) {
		return compare(Opcode::Ne, lhs, rhs);
	}
	Node* Function::slt(Node* lhs, Node* rhs) {
		return compare(Opcode::Slt, lhs, rhs);
	}
	Node* Function::sle(Node* lhs, Node* rhs) {
		return compare(Opcode::Sle, lhs, rhs);
	}
	Node* Function::sgt(Node* lhs, Node* rhs) {
		return compare(Opcode::Slt, rhs, lhs);
	}
	Node* Function::sge(Node* lhs, Node* rhs) {
		return compare(Opcode::Sle, rhs, lhs);
	}
	Node* Function::ult(Node* lhs, Node* rhs) {
		return compare(Opcode::Ult, lhs, rhs);
	}
	Node* Function::ule(Node* lhs, Node* rhs) {
		return compare(Opcode::Ule, lhs, rhs);
	}
	Node* Function::ugt(Node* lhs, Node* rhs) {
		return compare(Opcode::Ult, rhs, lhs);
	}
	Node* Function::uge(Node* lhs, Node* rhs) {
		return compare(Opcode::Ule, rhs, lhs);
	}

	Node* Function::convert(Opcode op, Node* operand, Type* destType) {
		return create<ConvertNode>(op, destType, operand);
	}
	Node* Function::trunc(Node* operand, Type* destType) {
		return convert(Opcode::Trunc, operand, destType);
	}
	Node* Function::sext(Node* operand, Type* destType) {
		return convert(Opcode::SExt, operand, destType);
	}
	Node* Function::zext(Node* operand, Type* destType) {
		return convert(Opcode::ZExt, operand, destType);
	}

	Node* Function::load(Type* valueType, Node* pointer) {
		return create<LoadNode>(valueType, control(), readVar(memVar), pointer);
	}

	void Function::store(Node* pointer, Node* value) {
		Node* nm =
				create<StoreNode>(memTy(), control(), readVar(memVar), pointer, value);
		writeVar(memVar, nm);
	}

	Node* Function::call(const String& callee, Type* retType,
											 const List<Node*>& args) {
		List<Type*> elems{ctrlTy(), memTy()};
		if (retType)
			elems.push_back(retType);
		Type* tupleTy = mod->getTuple(elems);

		List<Node*> ins{control(), readVar(memVar)};
		for (Node* a : args)
			ins.push_back(a);

		CallNode* c = create<CallNode>(tupleTy, callee, retType != nullptr, ins);
		cur->ctrl = proj(c, CallNode::controlProjIndex(), ctrlTy(), "ctrl");
		writeVar(memVar, proj(c, CallNode::memoryProjIndex(), memTy(), "mem"));
		if (retType)
			return proj(c, CallNode::valueProjIndex(), retType, "ret");
		return nullptr;
	}

	IfNode* Function::iff(Node* predicate) {
		return create<IfNode>(mod->getTuple({ctrlTy(), ctrlTy()}), control(),
													predicate);
	}
	ProjNode* Function::proj(Node* tuple, U32 index, Type* type, String label) {
		return create<ProjNode>(type, tuple, index, std::move(label));
	}
	RegionNode* Function::region(const List<Node*>& preds) {
		return create<RegionNode>(ctrlTy(), preds);
	}
	PhiNode* Function::phi(Type* type, RegionNode* region,
												 const List<Node*>& values) {
		List<Node*> ins{region};
		for (Node* v : values)
			ins.push_back(v);
		return create<PhiNode>(type, ins);
	}

	Function::Block* Function::makeBlock(String name, B32 loopHeader) {
		Block* b = arena.make<Block>();
		b->name = std::move(name);
		b->loopHeader = loopHeader;
		blocks.push_back(b);
		if (loopHeader) {
			b->region = create<RegionNode>(ctrlTy(), List<Node*>{});
			b->region->setLoopHeader();
			b->ctrl = b->region;
			b->active = true;
		}
		return b;
	}

	Function::Block* Function::createBlock(String name) {
		return makeBlock(std::move(name), false);
	}
	Function::Block* Function::createLoopHeader(String name) {
		return makeBlock(std::move(name), true);
	}

	void Function::addEdge(Node* exitControl, Block* from, Block* to) {
		to->preds.push_back(exitControl);
		to->predBlocks.push_back(from);
		if (to->region)
			to->region->addInput(exitControl);
	}

	void Function::activateOnSeal(Block* block) {
		if (block->active)
			return; // loop headers and the entry are already active
		if (block->preds.size() >= 2) {
			block->region = create<RegionNode>(ctrlTy(), block->preds);
			block->ctrl = block->region;
			block->active = true;
		} else if (block->preds.size() == 1) {
			block->ctrl = block->preds[0]; // single predecessor needs no region
			block->active = true;
		}
		// zero predecessors: unreachable block; stays inactive
	}

	void Function::seal(Block* block) {
		activateOnSeal(block);
		for (auto& kv : block->incompletePhis)
			addPhiOperands(kv.first, kv.second, block);
		block->incompletePhis.clear();
		block->sealed = true;
	}

	void Function::setInsertBlock(Block* block) { cur = block; }

	void Function::jmp(Block* target) {
		addEdge(cur->ctrl, cur, target);
		cur->finished = true;
	}

	void Function::jumpif(Node* cond, Block* target) {
		IfNode* branch = iff(cond);
		Node* thenP = proj(branch, IfNode::thenProjIndex(), ctrlTy(), "then");
		Node* elseP = proj(branch, IfNode::elseProjIndex(), ctrlTy(), "else");
		addEdge(thenP, cur, target);
		// the false path falls through into a fresh continuation block
		Block* fall = createBlock("ft");
		addEdge(elseP, cur, fall);
		cur->finished = true;
		seal(fall);
		setInsertBlock(fall);
	}

	Function::Var Function::newVar(String name, Type* type) {
		varInfos.push_back({std::move(name), type});
		return (Var)(varInfos.size() - 1);
	}

	void Function::writeVar(Var var, Node* value) { cur->defs[var] = value; }
	Node* Function::readVar(Var var) { return readVariable(var, cur); }

	Node* Function::readVariable(Var var, Block* block) {
		auto it = block->defs.find(var);
		if (it != block->defs.end())
			return it->second;
		return readVariableRecursive(var, block);
	}

	PhiNode* Function::newIncompletePhi(Var var, Block* block) {
		return create<PhiNode>(varInfos[var].ty, List<Node*>{block->region});
	}

	Node* Function::readVariableRecursive(Var var, Block* block) {
		Node* val = nullptr;
		if (!block->sealed) {
			// unsealed (loop header): an incomplete phi, completed at seal()
			PhiNode* p = newIncompletePhi(var, block);
			block->incompletePhis[var] = p;
			val = p;
		} else if (block->preds.size() == 1) {
			val = readVariable(var, block->predBlocks[0]);
		} else {
			PhiNode* p = newIncompletePhi(var, block);
			block->defs[var] = p; // break cycles before reading predecessors
			val = addPhiOperands(var, p, block);
		}
		block->defs[var] = val;
		return val;
	}

	Node* Function::addPhiOperands(Var var, PhiNode* phi, Block* block) {
		for (Block* p : block->predBlocks)
			phi->addInput(readVariable(var, p)); // aligns with region inputs by order
		return tryRemoveTrivialPhi(phi);
	}

	void Function::replacePhiEverywhere(PhiNode* phi, Node* with) {
		phi->replaceAllUsesWith(with);
		for (Block* b : blocks)
			for (auto& kv : b->defs)
				if (kv.second == phi)
					kv.second = with;
	}

	Node* Function::tryRemoveTrivialPhi(PhiNode* phi) {
		Node* same = nullptr;
		for (U32 i = 0, e = phi->getValueCount(); i < e; ++i) {
			Node* op = phi->getValue(i);
			if (op == phi || op == same)
				continue;
			if (same)
				return phi; // two distinct operands -> not trivial
			same = op;
		}
		if (!same)
			return phi; // no real operand yet; leave it

		List<PhiNode*> phiUsers;
		for (Node* u : phi->getUsers())
			if (u != phi && u->getOpcode() == Opcode::Phi)
				phiUsers.push_back(cast<PhiNode>(u));

		replacePhiEverywhere(phi, same);

		for (PhiNode* pu : phiUsers)
			tryRemoveTrivialPhi(pu);
		return same;
	}

	Function::Var Function::declareLocal(String name, Node* init) {
		Var v = newVar(std::move(name), init->getType());
		writeVar(v, init);
		return v;
	}
	Node* Function::get(Var var) { return readVar(var); }
	void Function::set(Var var, Node* value) { writeVar(var, value); }
	const String& Function::localName(Var var) const {
		return varInfos[var].name;
	}
	U32 Function::numLocals() const { return (U32)varInfos.size(); }

	void Function::loop(const std::function<void()>& bodyFn) {
		Block* h = createLoopHeader("loop.header");
		Block* x = createBlock("loop.exit");

		jmp(h);
		loopStack.push_back({h, x});
		setInsertBlock(h);
		if (bodyFn)
			bodyFn();
		if (!blockFinished())
			jmp(h);
		loopStack.pop_back();

		seal(h);
		seal(x);
		if (x->active)
			setInsertBlock(x);
	}

	void Function::break_() { jmp(loopStack.back().exit); }
	void Function::continue_() { jmp(loopStack.back().header); }
	void Function::breakIf(Node* cond) { jumpif(cond, loopStack.back().exit); }
	void Function::continueIf(Node* cond) {
		jumpif(cond, loopStack.back().header);
	}

	void Function::ret(Node* value) {
		Node* r = create<ReturnNode>(
				ctrlTy(), List<Node*>{control(), readVar(memVar), value});
		stop->addInput(r);
		cur->finished = true;
	}

	void Function::retVoid() {
		Node* r =
				create<ReturnNode>(ctrlTy(), List<Node*>{control(), readVar(memVar)});
		stop->addInput(r);
		cur->finished = true;
	}

	Node* Function::NodeIterator::operator*() const { return *it; }
	Function::NodeIterator& Function::NodeIterator::operator++() {
		++it;
		return *this;
	}
	B32 Function::NodeIterator::operator!=(const NodeIterator& other) const {
		return it != other.it;
	}

	Function::NodeIterator Function::begin() const { return {nodes.begin()}; }
	Function::NodeIterator Function::end() const { return {nodes.end()}; }
	U32 Function::size() const { return (U32)nodes.size(); }

	U32 Function::eliminateDeadNodes(B32 includeControl) {
		U32 removed = 0;
		B32 changed = true;
		while (changed) {
			changed = false;
			for (auto it = nodes.begin(); it != nodes.end();) {
				Node* n = *it;
				B32 dead = !n->hasUsers() && !n->hasSideEffects() &&
									 (includeControl || !n->isCFG()) && n != start && n != stop;
				if (dead) {
					// drop outgoing edges so input user-lists stay correct
					while (n->getInputCount() > 0)
						n->removeInput(n->getInputCount() - 1);
					it = nodes.erase(it);
					++removed;
					changed = true;
				} else {
					++it;
				}
			}
		}
		return removed;
	}

	U32 Function::allocateId() { return nextId++; }
} // namespace rat
