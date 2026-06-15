#ifndef RAT_IR_NODE_H
#define RAT_IR_NODE_H

#include "Core.h"
#include "IR/Opcode.h"
#include "IR/Type.h"

namespace rat {
	struct Function;

	struct Node {
		Node(Function& fn, Opcode op, Type* type, const List<Node*>& inputs);
		virtual ~Node();

		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		Opcode getOpcode() const;
		const char* getMnemonic() const;
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
		void replaceInput(Node* old, Node* replacement);

		void replaceAllUsesWith(Node* value);

		B32 isCFG() const;
		B32 hasSideEffects() const;
		B32 isCommutative() const;

		Node* getControlInput() const;

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
		ProjNode(Function& fn, Type* type, Node* tuple, U32 index,
						 String label = "");

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
		LoadNode(Function& fn, Type* valueType, Node* control, Node* memory,
						 Node* pointer);

		Node* getControl() const;
		Node* getMemory() const;
		Node* getPointer() const;
	};

	struct StoreNode : Node {
		StoreNode(Function& fn, Type* memoryType, Node* control, Node* memory,
							Node* pointer, Node* value);

		Node* getControl() const;
		Node* getMemory() const;
		Node* getPointer() const;
		Node* getValue() const;
	};

	struct CallNode : Node {
		CallNode(Function& fn, Type* tupleType, String callee, B32 returnsValue,
						 const List<Node*>& controlMemoryArgs);

		const String& getCallee() const;
		Node* getControl() const;
		Node* getMemory() const;
		U32 getArgCount() const;
		Node* getArg(U32 index) const;
		B32 returnsValue() const;

		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 valueProjIndex() { return 2; }

	private:
		String callee;
		B32 hasReturnValue;
	};
} // namespace rat

#endif
