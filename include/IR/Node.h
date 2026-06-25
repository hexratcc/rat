#ifndef RAT_IR_NODE_H
#define RAT_IR_NODE_H

#include "Core.h"
#include "IR/Opcode.h"
#include "IR/Type.h"

namespace rat {
	struct Function;
	struct ProjNode;

	struct Node {
		Node(Function& fn, Opcode op, Type* type, const List<Node*>& inputs);
		virtual ~Node();

		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		Opcode getOpcode() const;
		const C8* getMnemonic() const;
		Type* getType() const;
		U32 getId() const;
		Function& getFunction() const;

		U32 getInputCount() const;
		Node* getInput(U32 index) const;
		const List<Node*>& getInputs() const;

		const List<Node*>& getUsers() const;
		U32 getUserCount() const;
		B32 hasUsers() const;

		void addInput(Node* value);
		void setInput(U32 index, Node* value);
		void removeInput(U32 index);
		void clearInputs();
		void replaceInput(Node* old, Node* replacement);

		void replaceAllUsesWith(Node* value);

		B32 isCFG() const;
		B32 hasSideEffects() const;
		B32 isCommutative() const;

		Node* getControlInput() const;

		ProjNode* projection(U32 index) const;
	protected:
		void removeUser(Node* user);

		Opcode op;
		Type* ty;
		U32 id;
		Function* fn;
		List<Node*> inputs; // ordered defs; may contain null placeholders
		List<Node*> users;	// reverse edges; one entry per using operand
	};

	struct StartNode : Node {
		StartNode(Function& fn, Type* tupleType, U32 paramCount);

		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 paramProjIndex(U32 index) { return 2 + index; }
		U32 getParamCount() const;
	private:
		U32 paramCount;
	};

	struct StopNode : Node {
		StopNode(Function& fn, Type* controlType);
	};

	struct ReturnNode : Node {
		ReturnNode(Function& fn, Type* controlType, const List<Node*>& inputs);

		Node* getControl() const;
		Node* getMemory() const;
		B32 hasValue() const;
		Node* getValue() const;
	};

	struct RegionNode : Node {
		RegionNode(Function& fn, Type* controlType, const List<Node*>& preds);

		U32 getPredecessorCount() const;
		Node* getPredecessor(U32 index) const;

		B32 isLoopHeader() const;
		void setLoopHeader(B32 value = true);
	private:
		B32 loopHeader = false;
	};

	struct IfNode : Node {
		IfNode(Function& fn, Type* tupleType, Node* control, Node* predicate);

		Node* getControl() const;
		Node* getPredicate() const;
		static constexpr U32 thenProjIndex() { return 0; }
		static constexpr U32 elseProjIndex() { return 1; }
	};

	struct ProjNode : Node {
		ProjNode(Function& fn, Type* type, Node* tuple, U32 index, String label = "");

		Node* getProducer() const;
		U32 getIndex() const;
		const String& getLabel() const;
	private:
		U32 index;
		String label;
	};

	struct PhiNode : Node {
		PhiNode(Function& fn, Type* type, const List<Node*>& inputs);

		RegionNode* getRegion() const;
		U32 getValueCount() const;
		Node* getValue(U32 index) const;
		void setValue(U32 index, Node* value);
	};

	struct ConstantNode : Node {
		ConstantNode(Function& fn, Type* type, I64 value);

		I64 getValue() const;
	private:
		I64 value;
	};

	struct BinaryNode : Node {
		BinaryNode(Function& fn, Opcode op, Type* type, Node* lhs, Node* rhs);

		Node* getLHS() const;
		Node* getRHS() const;
	};

	struct UnaryNode : Node {
		UnaryNode(Function& fn, Opcode op, Type* type, Node* operand);

		Node* getOperand() const;
	};

	struct CompareNode : Node {
		CompareNode(Function& fn, Opcode op, Type* boolType, Node* lhs, Node* rhs);

		Node* getLHS() const;
		Node* getRHS() const;
	};

	struct ConvertNode : Node {
		ConvertNode(Function& fn, Opcode op, Type* destType, Node* operand);

		Node* getOperand() const;
	};

	struct LoadNode : Node {
		LoadNode(Function& fn, Type* valueType, Node* control, Node* memory, Node* pointer);

		Node* getControl() const;
		Node* getMemory() const;
		Node* getPointer() const;
	};

	struct StoreNode : Node {
		StoreNode(
				Function& fn, Type* memoryType, Node* control, Node* memory, Node* pointer, Node* value);

		Node* getControl() const;
		Node* getMemory() const;
		Node* getPointer() const;
		Node* getValue() const;
	};

	struct CallNode : Node {
		CallNode(Function& fn,
						 Type* tupleType,
						 String callee,
						 B32 returnsValue,
						 const List<Node*>& controlMemoryArgs,
						 B32 indirect = false);

		const String& getCallee() const;
		void setCallee(String name);
		Node* getControl() const;
		Node* getMemory() const;
		U32 getArgCount() const;
		Node* getArg(U32 index) const;
		B32 returnsValue() const;
		B32 isIndirect() const;
		Node* getTarget() const;

		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 valueProjIndex() { return 2; }
	private:
		String callee;
		B32 hasReturnValue;
		B32 indirect;
	};

	struct GlobalNode : Node {
		GlobalNode(Function& fn, Type* ptrType, String symbol);

		const String& getSymbol() const;
	private:
		String symbol;
	};

	struct AllocNode : Node {
		AllocNode(Function& fn, Type* ptrType, Type* allocType);
		AllocNode(Function& fn, Type* ptrType, Type* allocType, Node* size);

		Type* getAllocType() const;
		Node* getSizeOperand() const;
		B32 isVariableSized() const { return getInputCount() > 0; }
	private:
		Type* allocType;
	};

	namespace detail {
		template <typename T> B32 nodeIsa(const Node* n);
		// clang-format off
		template <> inline B32 nodeIsa<StartNode>(const Node* n)    { return n->getOpcode() == Opcode::Start; }
		template <> inline B32 nodeIsa<StopNode>(const Node* n)     { return n->getOpcode() == Opcode::Stop; }
		template <> inline B32 nodeIsa<ReturnNode>(const Node* n)   { return n->getOpcode() == Opcode::Return; }
		template <> inline B32 nodeIsa<RegionNode>(const Node* n)   { return n->getOpcode() == Opcode::Region; }
		template <> inline B32 nodeIsa<IfNode>(const Node* n)       { return n->getOpcode() == Opcode::If; }
		template <> inline B32 nodeIsa<ProjNode>(const Node* n)     { return n->getOpcode() == Opcode::Proj; }
		template <> inline B32 nodeIsa<PhiNode>(const Node* n)      { return n->getOpcode() == Opcode::Phi; }
		template <> inline B32 nodeIsa<ConstantNode>(const Node* n) { return n->getOpcode() == Opcode::Constant; }
		template <> inline B32 nodeIsa<BinaryNode>(const Node* n)   { return isBinaryOpcode(n->getOpcode()); }
		template <> inline B32 nodeIsa<UnaryNode>(const Node* n)    { return isUnaryOpcode(n->getOpcode()); }
		template <> inline B32 nodeIsa<CompareNode>(const Node* n)  { return isCompareOpcode(n->getOpcode()); }
		template <> inline B32 nodeIsa<ConvertNode>(const Node* n)  { return isConvertOpcode(n->getOpcode()); }
		template <> inline B32 nodeIsa<LoadNode>(const Node* n)     { return n->getOpcode() == Opcode::Load; }
		template <> inline B32 nodeIsa<StoreNode>(const Node* n)    { return n->getOpcode() == Opcode::Store; }
		template <> inline B32 nodeIsa<CallNode>(const Node* n)     { return n->getOpcode() == Opcode::Call; }
		template <> inline B32 nodeIsa<GlobalNode>(const Node* n)   { return n->getOpcode() == Opcode::Global; }
		template <> inline B32 nodeIsa<AllocNode>(const Node* n)    { return n->getOpcode() == Opcode::Alloc; }
		// clang-format on
	} // namespace detail

	template <typename T> B32 isa(const Node* n) { return n && detail::nodeIsa<T>(n); }
	template <typename T> T* cast(Node* n) { return static_cast<T*>(n); }
	template <typename T> const T* cast(const Node* n) { return static_cast<const T*>(n); }
	template <typename T> T* dyn_cast(Node* n) { return isa<T>(n) ? static_cast<T*>(n) : nullptr; }
	template <typename T> const T* dyn_cast(const Node* n) {
		return isa<T>(n) ? static_cast<const T*>(n) : nullptr;
	}

	template <typename T> List<T*> usersOfType(const Node* n) {
		List<T*> out;
		for(Node* u : n->getUsers())
			if(T* t = dyn_cast<T>(u))
				out.push_back(t);
		return out;
	}

	inline B32 isControlNode(const Node* n) {
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

	Node* cloneShell(Function& into, const Node* n);
	String nodeSignature(const Node* n);
} // namespace rat

#endif
