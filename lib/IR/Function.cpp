#include "IR/Function.h"

#include "IR/Module.h"

namespace rat {
	Module& Function::getModule() const { return *mod; }
	TypeContext& Function::types() const { return *mod; }
	const String& Function::getName() const { return name; }

	U32 Function::getParamCount() const { return (U32)paramTypes.size(); }
	Type* Function::getParamType(U32 index) const { return paramTypes[index]; }
	Type* Function::getReturnType() const { return retType; }
	B32 Function::returnsValue() const { return retType != nullptr; }

	StartNode* Function::getStart() const { return start; }
	StopNode* Function::getStop() const { return stop; }

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

		// project the control and memory tokens out of start so the body can thread
		// them through returns and memory operations
		control = create<ProjNode>(tc.getControl(), start,
															 StartNode::controlProjIndex(), "ctrl");
		memory = create<ProjNode>(tc.getMemory(), start,
															StartNode::memoryProjIndex(), "mem");
	}

	Node* Function::param(U32 index) {
		if (paramCache.empty())
			paramCache.resize(getParamCount(), nullptr);
		if (!paramCache[index]) {
			paramCache[index] = create<ProjNode>(paramTypes[index], start,
																					 StartNode::paramProjIndex(index),
																					 "arg" + std::to_string(index));
		}
		return paramCache[index];
	}

	Node* Function::constInt(Type* type, I64 value) {
		return create<ConstantNode>(type, value);
	}

	Node* Function::constBool(B32 value) {
		return create<ConstantNode>(types().getBool(), value ? 1 : 0);
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

	void Function::ret(Node* value) {
		Node* r = create<ReturnNode>(types().getControl(),
																 List<Node*>{control, memory, value});
		stop->addInput(r);
	}

	void Function::retVoid() {
		Node* r =
				create<ReturnNode>(types().getControl(), List<Node*>{control, memory});
		stop->addInput(r);
	}

	Node* Function::NodeIterator::operator*() const { return it->get(); }
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

	U32 Function::allocateId() { return nextId++; }
} // namespace rat
