#include "IR/Node.h"

#include "IR/Function.h"

namespace rat {
	Node::Node(Function& fn, Opcode op, Type* type, const List<Node*>& inputs)
	: op(op),
		ty(type),
		fn(&fn),
		inputs(inputs) {
		id = fn.allocateId();
		for(Node* in : this->inputs)
			if(in)
				in->users.push_back(this);
	}

	Node::~Node() = default;

	Opcode Node::getOpcode() const { return op; }
	const C8* Node::getMnemonic() const { return getOpcodeMnemonic(op); }
	Type* Node::getType() const { return ty; }
	U32 Node::getId() const { return id; }
	Function& Node::getFunction() const { return *fn; }

	U32 Node::getInputCount() const { return (U32)inputs.size(); }
	Node* Node::getInput(U32 index) const { return inputs[index]; }
	const List<Node*>& Node::getInputs() const { return inputs; }

	const List<Node*>& Node::getUsers() const { return users; }
	U32 Node::getUserCount() const { return (U32)users.size(); }
	B32 Node::hasUsers() const { return !users.empty(); }

	void Node::addInput(Node* value) {
		inputs.push_back(value);
		if(value)
			value->users.push_back(this);
	}

	void Node::removeUser(Node* user) {
		// remove a single occurrence: each using operand contributes one entry
		auto it = std::find(users.begin(), users.end(), user);
		if(it != users.end())
			users.erase(it);
	}

	void Node::setInput(U32 index, Node* value) {
		Node* old = inputs[index];
		if(old == value)
			return;
		if(old)
			old->removeUser(this);
		inputs[index] = value;
		if(value)
			value->users.push_back(this);
	}

	void Node::removeInput(U32 index) {
		if(inputs[index])
			inputs[index]->removeUser(this);
		inputs.erase(inputs.begin() + index);
	}

	void Node::clearInputs() {
		while(getInputCount() > 0)
			removeInput(getInputCount() - 1);
	}

	void Node::replaceInput(Node* old, Node* replacement) {
		for(U32 i = 0, e = getInputCount(); i < e; ++i)
			if(inputs[i] == old)
				setInput(i, replacement);
	}

	void Node::replaceAllUsesWith(Node* value) {
		// snapshot users
		List<Node*> snapshot = users;
		for(Node* user : snapshot)
			for(U32 i = 0, e = user->getInputCount(); i < e; ++i)
				if(user->getInput(i) == this)
					user->setInput(i, value);
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
		for(Node* u : users)
			if(ProjNode* p = dyn_cast<ProjNode>(u))
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

	ProjNode::ProjNode(Function& fn, Type* type, Node* tuple, U32 index, String label)
	: Node(fn, Opcode::Proj, type, {tuple}),
		index(index),
		label(std::move(label)) {}

	Node* ProjNode::getProducer() const { return getInput(0); }
	U32 ProjNode::getIndex() const { return index; }
	const String& ProjNode::getLabel() const { return label; }

	PhiNode::PhiNode(Function& fn, Type* type, const List<Node*>& inputs)
	: Node(fn, Opcode::Phi, type, inputs) {}

	RegionNode* PhiNode::getRegion() const { return cast<RegionNode>(getInput(0)); }
	U32 PhiNode::getValueCount() const { return getInputCount() - 1; }
	Node* PhiNode::getValue(U32 index) const { return getInput(1 + index); }
	void PhiNode::setValue(U32 index, Node* value) { setInput(1 + index, value); }

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

	B32 isControlNode(const Node* n) {
		switch(n->getOpcode()) {
		case Opcode::Start:
		case Opcode::Stop:
		case Opcode::Return:
		case Opcode::Region:
		case Opcode::If:
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

	String nodeSignature(const Node* n) {
		String key =
				std::to_string((U32)n->getOpcode()) + "|" + std::to_string((uintptr_t)n->getType()) + "|";

		// immediate (non-edge) fields, by opcode
		switch(n->getOpcode()) {
		case Opcode::Constant:
			key += "c" + std::to_string(cast<ConstantNode>(n)->getValue());
			break;
		case Opcode::Global:
			key += "g" + cast<GlobalNode>(n)->getSymbol();
			break;
		case Opcode::Proj: {
			const ProjNode* p = cast<ProjNode>(n);
			key += "p" + std::to_string(p->getIndex()) + ":" + p->getLabel();
			break;
		}
		case Opcode::Call:
			key += "f" + cast<CallNode>(n)->getCallee();
			break;
		case Opcode::Alloc:
			key += "a" + std::to_string((uintptr_t)cast<AllocNode>(n)->getAllocType());
			break;
		default:
			break;
		}
		key += "|";

		// sort for commutative opcodes
		List<U32> ops;
		ops.reserve(n->getInputCount());
		for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
			ops.push_back(n->getInput(i)->getId());
		if(n->isCommutative())
			std::sort(ops.begin(), ops.end());
		for(U32 id : ops)
			key += std::to_string(id) + ",";
		return key;
	}
} // namespace rat
