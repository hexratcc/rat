#include "IR/Node.h"

#include "IR/Function.h"

namespace rat {
	Node::Node(Function& fn, Opcode op, Type* type, const List<Node*>& inputs)
	: op(op),
		ty(type),
		fn(&fn) {
		id = fn.allocateId();
		inputCount = (U32)inputs.size();
		if(inputCount) {
			inputCap = inputCount;
			this->inputs = fn.allocEdges(inputCap);
			std::memcpy(this->inputs, inputs.data(), sizeof(Node*) * inputCount);
			for(U32 i = 0; i < inputCount; ++i)
				if(Node* in = this->inputs[i])
					in->addUser(this);
		}
	}

	Opcode Node::getOpcode() const { return op; }
	const C8* Node::getMnemonic() const { return getOpcodeMnemonic(op); }
	Type* Node::getType() const { return ty; }
	U32 Node::getId() const { return id; }
	Function& Node::getFunction() const { return *fn; }

	U32 Node::getInputCount() const { return inputCount; }
	Node* Node::getInput(U32 index) const { return inputs[index]; }

	NodeSpan Node::getUsers() const { return NodeSpan{users, userCount}; }
	B32 Node::hasUsers() const { return userCount != 0; }

	void Node::addUser(Node* user) {
		if(userCount == userCap) {
			U32 cap = userCap ? userCap * 2 : 4;
			Node** grown = fn->allocEdges(cap);
			if(userCount)
				std::memcpy(grown, users, sizeof(Node*) * userCount);
			users = grown;
			userCap = cap;
		}
		users[userCount++] = user;
	}

	void Node::growInputs(U32 needed) {
		if(needed <= inputCap)
			return;
		U32 cap = inputCap ? inputCap * 2 : 4;
		if(cap < needed)
			cap = needed;
		Node** grown = fn->allocEdges(cap);
		if(inputCount)
			std::memcpy(grown, inputs, sizeof(Node*) * inputCount);
		inputs = grown;
		inputCap = cap;
	}

	void Node::addInput(Node* value) {
		growInputs(inputCount + 1);
		inputs[inputCount++] = value;
		if(value)
			value->addUser(this);
	}

	void Node::removeUser(Node* user) {
		// remove a single occurrence: each using operand contributes one entry
		for(U32 i = 0; i < userCount; ++i)
			if(users[i] == user) {
				users[i] = users[--userCount];
				return;
			}
	}

	void Node::setInput(U32 index, Node* value) {
		Node* old = inputs[index];
		if(old == value)
			return;
		if(old)
			old->removeUser(this);
		inputs[index] = value;
		if(value)
			value->addUser(this);
	}

	void Node::removeInput(U32 index) {
		if(inputs[index])
			inputs[index]->removeUser(this);
		for(U32 i = index + 1; i < inputCount; ++i)
			inputs[i - 1] = inputs[i];
		--inputCount;
	}

	void Node::clearInputs() {
		for(U32 i = 0; i < inputCount; ++i)
			if(inputs[i])
				inputs[i]->removeUser(this);
		inputCount = 0;
	}

	void Node::replaceAllUsesWith(Node* value) {
		if(value == this)
			return;
		for(U32 u = 0; u < userCount; ++u) {
			Node* user = users[u];
			for(U32 i = 0, e = user->inputCount; i < e; ++i)
				if(user->inputs[i] == this) {
					user->inputs[i] = value;
					if(value)
						value->addUser(user);
				}
		}
		userCount = 0;
	}

	B32 Node::isCFG() const { return getOpcodeInfo(op).isCFG; }

	B32 Node::hasSideEffects() const { return getOpcodeInfo(op).hasSideEffects; }

	B32 Node::isCommutative() const { return getOpcodeInfo(op).isCommutative; }

	Node* Node::getControlInput() const {
		// for Phi, input[0] is the controlling Region
		I32 slot = getOpcodeInfo(op).controlInputIndex;
		if(slot < 0 || (U32)slot >= getInputCount())
			return nullptr;
		return getInput((U32)slot);
	}

	ProjNode* Node::projection(U32 index) const {
		for(U32 i = 0; i < userCount; ++i)
			if(ProjNode* p = dyn_cast<ProjNode>(users[i]))
				if(p->getIndex() == index)
					return p;
		return nullptr;
	}

	StartNode::StartNode(Function& fn, Type* tupleType, U32 paramCount)
	: Node(fn, Opcode::Start, tupleType, {}),
		paramCount(paramCount) {}

	U32 StartNode::getParamCount() const { return paramCount; }

	StopNode::StopNode(Function& fn, Type* controlType)
	: Node(fn, Opcode::Stop, controlType, {}) {}

	ReturnNode::ReturnNode(Function& fn, Type* controlType, const List<Node*>& inputs)
	: Node(fn, Opcode::Return, controlType, inputs) {}

	Node* ReturnNode::getControl() const { return getInput(0); }
	Node* ReturnNode::getMemory() const { return getInput(1); }
	B32 ReturnNode::hasValue() const { return getInputCount() > 2; }
	Node* ReturnNode::getValue() const { return hasValue() ? getInput(2) : nullptr; }

	RegionNode::RegionNode(Function& fn, Type* controlType, const List<Node*>& preds)
	: Node(fn, Opcode::Region, controlType, preds) {}

	U32 RegionNode::getPredecessorCount() const { return getInputCount(); }
	Node* RegionNode::getPredecessor(U32 index) const { return getInput(index); }
	B32 RegionNode::isLoopHeader() const { return loopHeader; }
	void RegionNode::setLoopHeader(B32 value) { loopHeader = value; }

	IfNode::IfNode(Function& fn, Type* tupleType, Node* control, Node* predicate)
	: Node(fn, Opcode::If, tupleType, {control, predicate}) {}

	Node* IfNode::getControl() const { return getInput(0); }
	Node* IfNode::getPredicate() const { return getInput(1); }

	SwitchNode::SwitchNode(Function& fn, Type* tupleType, Node* control, Node* selector)
	: Node(fn, Opcode::Switch, tupleType, {control, selector}) {}

	Node* SwitchNode::getControl() const { return getInput(0); }
	Node* SwitchNode::getSelector() const { return getInput(1); }
	U32 SwitchNode::getSlotCount() const { return getType()->getTupleElementCount(); }

	ProjNode::ProjNode(Function& fn, Type* type, Node* tuple, U32 index, const String& label)
	: Node(fn, Opcode::Proj, type, {tuple}),
		index(index),
		label(fn.internString(label.c_str(), label.size())) {}

	ProjNode::ProjNode(Function& fn, Type* type, Node* tuple, U32 index, const C8* label)
	: Node(fn, Opcode::Proj, type, {tuple}),
		index(index),
		label(fn.internString(label, label ? std::strlen(label) : 0)) {}

	Node* ProjNode::getProducer() const { return getInput(0); }
	U32 ProjNode::getIndex() const { return index; }
	const C8* ProjNode::getLabel() const { return label; }

	PhiNode::PhiNode(Function& fn, Type* type, const List<Node*>& inputs)
	: Node(fn, Opcode::Phi, type, inputs) {}

	RegionNode* PhiNode::getRegion() const { return cast<RegionNode>(getInput(0)); }
	U32 PhiNode::getValueCount() const { return getInputCount() - 1; }
	Node* PhiNode::getValue(U32 index) const { return getInput(1 + index); }

	ConstantNode::ConstantNode(Function& fn, Type* type, I64 value)
	: Node(fn, Opcode::Constant, type, {}),
		value(value) {}

	I64 ConstantNode::getValue() const { return value; }

	BinaryNode::BinaryNode(Function& fn, Opcode op, Type* type, Node* lhs, Node* rhs)
	: Node(fn, op, type, {lhs, rhs}) {}

	Node* BinaryNode::getLHS() const { return getInput(0); }
	Node* BinaryNode::getRHS() const { return getInput(1); }

	UnaryNode::UnaryNode(Function& fn, Opcode op, Type* type, Node* operand)
	: Node(fn, op, type, {operand}) {}

	Node* UnaryNode::getOperand() const { return getInput(0); }

	CompareNode::CompareNode(Function& fn, Opcode op, Type* boolType, Node* lhs, Node* rhs)
	: Node(fn, op, boolType, {lhs, rhs}) {}

	Node* CompareNode::getLHS() const { return getInput(0); }
	Node* CompareNode::getRHS() const { return getInput(1); }

	ConvertNode::ConvertNode(Function& fn, Opcode op, Type* destType, Node* operand)
	: Node(fn, op, destType, {operand}) {}

	Node* ConvertNode::getOperand() const { return getInput(0); }

	LoadNode::LoadNode(Function& fn, Type* valueType, Node* control, Node* memory, Node* pointer)
	: Node(fn, Opcode::Load, valueType, {control, memory, pointer}) {}

	Node* LoadNode::getControl() const { return getInput(0); }
	Node* LoadNode::getMemory() const { return getInput(1); }
	Node* LoadNode::getPointer() const { return getInput(2); }

	StoreNode::StoreNode(
			Function& fn, Type* memoryType, Node* control, Node* memory, Node* pointer, Node* value)
	: Node(fn, Opcode::Store, memoryType, {control, memory, pointer, value}) {}

	Node* StoreNode::getControl() const { return getInput(0); }
	Node* StoreNode::getMemory() const { return getInput(1); }
	Node* StoreNode::getPointer() const { return getInput(2); }
	Node* StoreNode::getValue() const { return getInput(3); }

	CallNode::CallNode(Function& fn,
										 Type* tupleType,
										 String callee,
										 B32 returnsValue,
										 const List<Node*>& controlMemoryArgs,
										 B32 indirect)
	: Node(fn, Opcode::Call, tupleType, controlMemoryArgs),
		callee(std::move(callee)),
		hasReturnValue(returnsValue),
		indirect(indirect) {}

	const String& CallNode::getCallee() const { return callee; }
	void CallNode::setCallee(String name) { callee = std::move(name); }
	Node* CallNode::getControl() const { return getInput(0); }
	Node* CallNode::getMemory() const { return getInput(1); }
	// indirect calls reserve one extra input (index 2) for the target pointer
	U32 CallNode::getArgCount() const { return getInputCount() - 2 - (indirect ? 1 : 0); }
	Node* CallNode::getArg(U32 index) const { return getInput(2 + (indirect ? 1 : 0) + index); }
	B32 CallNode::returnsValue() const { return hasReturnValue; }
	B32 CallNode::isIndirect() const { return indirect; }
	Node* CallNode::getTarget() const { return indirect ? getInput(2) : nullptr; }

	GlobalNode::GlobalNode(Function& fn, Type* ptrType, String symbol)
	: Node(fn, Opcode::Global, ptrType, {}),
		symbol(std::move(symbol)) {}

	const String& GlobalNode::getSymbol() const { return symbol; }

	AllocNode::AllocNode(Function& fn, Type* ptrType, Type* allocType)
	: Node(fn, Opcode::Alloc, ptrType, {}),
		allocType(allocType) {}

	AllocNode::AllocNode(Function& fn, Type* ptrType, Type* allocType, Node* size)
	: Node(fn, Opcode::Alloc, ptrType, {size}),
		allocType(allocType) {}

	Type* AllocNode::getAllocType() const { return allocType; }

	Node* AllocNode::getSizeOperand() const { return getInputCount() > 0 ? getInput(0) : nullptr; }

	B32 AllocNode::isVariableSized() const { return getInputCount() > 0; }

	SplatNode::SplatNode(Function& fn, Type* vecType, Node* scalar)
	: Node(fn, Opcode::Splat, vecType, {scalar}) {}

	Node* SplatNode::getScalar() const { return getInput(0); }

	ExtractNode::ExtractNode(Function& fn, Type* elemType, Node* vector, U32 lane)
	: Node(fn, Opcode::Extract, elemType, {vector}),
		lane(lane) {}

	Node* ExtractNode::getVector() const { return getInput(0); }
	U32 ExtractNode::getLane() const { return lane; }

	PackNode::PackNode(Function& fn, Type* vecType, const List<Node*>& lanes)
	: Node(fn, Opcode::Pack, vecType, lanes) {}

	U32 PackNode::getLaneCount() const { return getInputCount(); }
	Node* PackNode::getLane(U32 index) const { return getInput(index); }

	B32 isControlNode(const Node* n) {
		switch(n->getOpcode()) {
		case Opcode::Start:
		case Opcode::Stop:
		case Opcode::Return:
		case Opcode::Region:
		case Opcode::If:
		case Opcode::Switch:
			return true;
		case Opcode::Proj:
			return n->getType()->isControl();
		default:
			return false;
		}
	}

	Node* cloneShell(Function& into, const Node* n) {
		Opcode op = n->getOpcode();
		Type* t = n->getType();
		List<Node*> nulls(n->getInputCount(), nullptr);
		switch(op) {
		case Opcode::Start:
			return into.create<StartNode>(t, cast<StartNode>(n)->getParamCount());
		case Opcode::Stop: {
			StopNode* s = into.create<StopNode>(t);
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				s->addInput(nullptr);
			return s;
		}
		case Opcode::Return:
			return into.create<ReturnNode>(t, nulls);
		case Opcode::Region: {
			RegionNode* r = into.create<RegionNode>(t, nulls);
			r->setLoopHeader(cast<RegionNode>(n)->isLoopHeader());
			return r;
		}
		case Opcode::If:
			return into.create<IfNode>(t, nullptr, nullptr);
		case Opcode::Switch:
			return into.create<SwitchNode>(t, nullptr, nullptr);
		case Opcode::Proj: {
			const ProjNode* p = cast<ProjNode>(n);
			return into.create<ProjNode>(t, nullptr, p->getIndex(), p->getLabel());
		}
		case Opcode::Phi:
			return into.create<PhiNode>(t, nulls);
		case Opcode::Constant:
			return into.create<ConstantNode>(t, cast<ConstantNode>(n)->getValue());
		case Opcode::Load:
			return into.create<LoadNode>(t, nullptr, nullptr, nullptr);
		case Opcode::Store:
			return into.create<StoreNode>(t, nullptr, nullptr, nullptr, nullptr);
		case Opcode::Call: {
			const CallNode* c = cast<CallNode>(n);
			return into.create<CallNode>(t, c->getCallee(), c->returnsValue(), nulls, c->isIndirect());
		}
		case Opcode::Global:
			return into.create<GlobalNode>(t, cast<GlobalNode>(n)->getSymbol());
		case Opcode::Alloc: {
			const AllocNode* a = cast<AllocNode>(n);
			if(a->isVariableSized())
				return into.create<AllocNode>(t, a->getAllocType(), nullptr);
			return into.create<AllocNode>(t, a->getAllocType());
		}
		case Opcode::Splat:
			return into.create<SplatNode>(t, nullptr);
		case Opcode::Extract:
			return into.create<ExtractNode>(t, nullptr, cast<ExtractNode>(n)->getLane());
		case Opcode::Pack:
			return into.create<PackNode>(t, nulls);
		default:
			break;
		}
		switch(getOpClass(op)) {
		case OpClass::Binary:
			return into.create<BinaryNode>(op, t, nullptr, nullptr);
		case OpClass::Unary:
			return into.create<UnaryNode>(op, t, nullptr);
		case OpClass::Compare:
			return into.create<CompareNode>(op, t, nullptr, nullptr);
		case OpClass::Convert:
			return into.create<ConvertNode>(op, t, nullptr);
		case OpClass::None:
			break;
		}
		return nullptr;
	}

} // namespace rat
