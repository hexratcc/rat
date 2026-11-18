// a function body is a single graph of nodes, control flow, memory state, and data values are all
// just typed edges between nodes:
// - data:    ordinary values (int/float/ptr), free to float anywhere the schedule allows, order is
//            implied purely by dependencies
// - control: a token that threads through Start -> if/region/... -> return, nodes that must execute
//            at a particular point (load, store, call, terminators) take it as an input
// - memory:  the heap as an SSA value, store consumes one memory state and produces the next, load
//            consumes, so independent memory chains can reorder freely while dependent ones can't
// nodes that produce several results at once (start, if, call) have a tuple type, consumers reach
// individual elements through projection nodes. ie. an if yields two control projections
// (then/else), a call yields control, memory, and optionally a value.

#ifndef RAT_IR_NODE_H
#define RAT_IR_NODE_H

#include "core.h"
#include "ir/opcode.h"
#include "ir/type.h"

#include <optional>

namespace rat {
	struct Function;
	struct ProjNode;
	struct Node;

	struct NodeSpan {
		Node* const* ptr = nullptr;
		U32 count = 0;

		Node* const* begin() const { return ptr; }
		Node* const* end() const { return ptr + count; }
		U32 size() const { return count; }
		B32 empty() const { return count == 0; }
		Node* operator[](U32 i) const { return ptr[i]; }
	};

	struct Node {
		Node(Function& fn, Opcode op, Type* type, const List<Node*>& inputs);

		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		Opcode getOpcode() const { return op; }
		const C8* getMnemonic() const;
		Type* getType() const { return ty; }
		U32 getId() const { return id; }

		// index into the function's DebugBody locs, 0 = none
		U32 getLoc() const { return loc; }
		void setLoc(U32 l) { loc = l; }

		U32 getInputCount() const { return inputCount; }
		Node* getInput(U32 index) const { return inputs[index]; }

		NodeSpan getUsers() const { return NodeSpan{users, userCount}; }
		U32 getUserSlot(U32 index) const { return userSlots[index]; }
		B32 hasUsers() const { return userCount != 0; }

		// all edge mutation keeps the users index consistent
		void addInput(Node* value);
		void setInput(U32 index, Node* value);
		void removeInput(U32 index);
		void clearInputs();

		// rewrite every user's matching operand to point at value instead, this node keeps its own
		// inputs and typically becomes dead afterwards
		void replaceAllUsesWith(Node* value);

		B32 isCFG() const;
		B32 hasSideEffects() const;
		B32 isCommutative() const;

		// the users entry of input index points back at this node and slot
		B32 inputEdgeIndexed(U32 index) const;

		// the control anchor for anchored nodes
		Node* getControlInput() const;

		// find this node's projection or null
		ProjNode* projection(U32 index) const;
	protected:
		void detachInput(U32 index); // drop the users entry of a live slot
		void addUser(Node* user, U32 slot);
		void growInputs(U32 needed);

		Opcode op;
		U32 id; // unique per function
		Type* ty;
		Function* fn;
		Node** inputs = nullptr;	// ordered defs, may hold null placeholders
		Node** users = nullptr;		// reverse edges, one entry per using operand
		U32* inputPos = nullptr;	// index of this node's entry in inputs[i]->users
		U32* userSlots = nullptr; // slot of users[k] that points here
		U32 inputCount = 0;
		U32 inputCap = 0;
		U32 userCount = 0;
		U32 userCap = 0;
		U32 loc = 0;
	};

	// input 0 is the control anchor, input 1 the memory state read
	struct AnchoredNode : Node {
		using Node::Node;
		Node* getControl() const;
		Node* getMemory() const;
	};

	// a node class with one opcode, isa<T> asks T::hasOpcode
	template <Opcode Op, typename Base = Node> struct FixedOpcodeNode : Base {
		FixedOpcodeNode(Function& fn, Type* type, const List<Node*>& inputs)
		: Base(fn, Op, type, inputs) {}

		static B32 hasOpcode(Opcode op) { return op == Op; }
	};

	// function entry
	struct StartNode : FixedOpcodeNode<Opcode::Start> {
		StartNode(Function& fn, Type* tupleType);

		// tuple producer whose projections are the entry control token, the initial memory state, and
		// one projection per parameter
		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 paramProjIndex(U32 index) { return 2 + index; }
	};

	// function exit
	struct StopNode : FixedOpcodeNode<Opcode::Stop> {
		StopNode(Function& fn, Type* controlType);
	};

	struct ReturnNode : FixedOpcodeNode<Opcode::Return, AnchoredNode> {
		ReturnNode(Function& fn, Type* controlType, const List<Node*>& inputs);

		B32 hasValue() const;
		Node* getValue() const;
	};

	// control merge point, the only place data flow can merge too, each PhiNode names its Region and
	// carries one value per Region predecessor
	struct RegionNode : FixedOpcodeNode<Opcode::Region> {
		// one input per predecessor control edge
		RegionNode(Function& fn, Type* controlType, const List<Node*>& preds, B32 loopHeader = false);

		U32 getPredecessorCount() const;
		Node* getPredecessor(U32 index) const;

		B32 isLoopHeader() const;
	private:
		B32 loopHeader;
	};

	struct IfNode : FixedOpcodeNode<Opcode::If> {
		IfNode(Function& fn, Type* tupleType, Node* control, Node* predicate);

		Node* getControl() const;
		Node* getPredicate() const;
		static constexpr U32 thenProjIndex() { return 0; }
		static constexpr U32 elseProjIndex() { return 1; }
	};

	// multi-way branch on a dense selector, proj i is the slot i control edge
	struct SwitchNode : FixedOpcodeNode<Opcode::Switch> {
		SwitchNode(Function& fn, Type* tupleType, Node* control, Node* selector);

		Node* getControl() const;
		Node* getSelector() const;
		U32 getSlotCount() const;
	};

	// selects element from a tuple producer (Start, If, Call, Switch)
	struct ProjNode : FixedOpcodeNode<Opcode::Proj> {
		ProjNode(Function& fn, Type* type, Node* tuple, U32 index, const C8* label);

		Node* getProducer() const;
		U32 getIndex() const;
		const C8* getLabel() const;
	private:
		U32 index;
		const C8* label;
	};

	// SSA merge, memory phis merge memory states the same way data phis merge values
	struct PhiNode : FixedOpcodeNode<Opcode::Phi> {
		// inputs[0] = owning Region
		// inputs[1 ... n] = incoming values
		PhiNode(Function& fn, Type* type, const List<Node*>& inputs);

		RegionNode* getRegion() const;
		U32 getValueCount() const;
		Node* getValue(U32 index) const;
	};

	// integer or floating constant
	struct ConstantNode : FixedOpcodeNode<Opcode::Constant> {
		ConstantNode(Function& fn, Type* type, I64 value);

		I64 getValue() const;
	private:
		I64 value;
	};

	struct BinaryNode : Node {
		BinaryNode(Function& fn, Opcode op, Type* type, Node* lhs, Node* rhs);
		static B32 hasOpcode(Opcode op) { return isBinaryOpcode(op); }

		Node* getLHS() const;
		Node* getRHS() const;

		B32 isNsw() const { return nsw; }
		void setNsw(B32 v) { nsw = v; }
	private:
		B32 nsw = false;
	};

	struct UnaryNode : Node {
		UnaryNode(Function& fn, Opcode op, Type* type, Node* operand);
		static B32 hasOpcode(Opcode op) { return isUnaryOpcode(op); }

		Node* getOperand() const;
	};

	struct CompareNode : Node {
		CompareNode(Function& fn, Opcode op, Type* boolType, Node* lhs, Node* rhs);
		static B32 hasOpcode(Opcode op) { return isCompareOpcode(op); }

		Node* getLHS() const;
		Node* getRHS() const;
	};

	struct ConvertNode : Node {
		ConvertNode(Function& fn, Opcode op, Type* destType, Node* operand);
		static B32 hasOpcode(Opcode op) { return isConvertOpcode(op); }

		Node* getOperand() const;
	};

	// consumes a memory state but does not produce one, so loads on the same state may reorder
	// freely, the control anchor gives the schedule a latest legal position
	struct LoadNode : FixedOpcodeNode<Opcode::Load, AnchoredNode> {
		LoadNode(Function& fn, Type* valueType, Node* control, Node* memory, Node* pointer);

		Node* getPointer() const;

		// a volatile access is never removed, merged, forwarded or widened
		B32 isVolatile() const { return volatileAccess; }
		void setVolatile(B32 v) { volatileAccess = v; }
	private:
		B32 volatileAccess = false;
	};

	// produces the next memory state
	struct StoreNode : FixedOpcodeNode<Opcode::Store, AnchoredNode> {
		StoreNode(
				Function& fn, Type* memoryType, Node* control, Node* memory, Node* pointer, Node* value);

		Node* getPointer() const;
		Node* getValue() const;

		B32 isVolatile() const { return volatileAccess; }
		void setVolatile(B32 v) { volatileAccess = v; }
	private:
		B32 volatileAccess = false;
	};

	enum class AbiClass : U8 {
		None,
		Integer,
		Sse,
		Memory,
		X87
	};

	struct AbiRegs {
		U8 count = 0;
		AbiClass cls[2] = {};

		U32 countOf(AbiClass c) const;
	};

	struct ByvalArg {
		U32 bytes = 0;
		U32 align = 0;
	};

	struct CallAbi {
		List<std::optional<ByvalArg>> byval;
		AbiRegs ret;

		std::optional<ByvalArg> byvalAt(U32 index) const;
	};

	struct CallNode : FixedOpcodeNode<Opcode::Call, AnchoredNode> {
		// control, memory, [target if indirect,] args..
		CallNode(Function& fn,
						 Type* tupleType,
						 String callee,
						 const List<Node*>& controlMemoryArgs,
						 B32 indirect = false);

		const String& getCallee() const;
		U32 getArgCount() const;
		Node* getArg(U32 index) const;
		B32 returnsValue() const;
		B32 isIndirect() const;
		Node* getTarget() const;

		// whether the callee may be variadic
		B32 isVarArgs() const { return varArgs; }
		void setVarArgs(B32 v) { varArgs = v; }

		const CallAbi& getAbi() const { return abi; }
		void setAbi(CallAbi a) { abi = std::move(a); }

		// post-call
		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 valueProjIndex() { return 2; }
	private:
		String callee;
		B32 varArgs = true;
		B32 indirect;
		CallAbi abi;
	};

	// inline assembly
	struct AsmNode : FixedOpcodeNode<Opcode::Asm, AnchoredNode> {
		// control, memory, input operand values..
		AsmNode(Function& fn,
						Type* tupleType,
						String text,
						U32 outputCount,
						const List<Node*>& controlMemoryInputs);

		const String& getText() const;
		Node* getInputOperand(U32 index) const;
		U32 getOutputCount() const { return outputCount; }

		static constexpr U32 controlProjIndex() { return 0; }
		static constexpr U32 memoryProjIndex() { return 1; }
		static constexpr U32 outputProjIndex(U32 index) { return 2 + index; }
	private:
		String text;
		U32 outputCount;
	};

	// broadcast one scalar into every lane of a vector
	struct SplatNode : FixedOpcodeNode<Opcode::Splat> {
		SplatNode(Function& fn, Type* vecType, Node* scalar);

		Node* getScalar() const;
	};

	// read a single lane of a vector as a scalar
	struct ExtractNode : FixedOpcodeNode<Opcode::Extract> {
		ExtractNode(Function& fn, Type* elemType, Node* vector, U32 lane);

		Node* getVector() const;
		U32 getLane() const;
	private:
		U32 lane;
	};

	// build a vector from one scalar per lane
	struct PackNode : FixedOpcodeNode<Opcode::Pack> {
		PackNode(Function& fn, Type* vecType, const List<Node*>& lanes);

		U32 getLaneCount() const;
		Node* getLane(U32 index) const;
	};

	// branch-free choice between two already-computed values
	struct SelectNode : FixedOpcodeNode<Opcode::Select> {
		SelectNode(Function& fn, Type* type, Node* cond, Node* thenVal, Node* elseVal);

		Node* getCondition() const;
		Node* getTrue() const;
		Node* getFalse() const;
	};

	// lane i of the result is lane (sel >> 4i) & 15 of the lanes of first followed by those of second
	struct ShuffleNode : FixedOpcodeNode<Opcode::Shuffle> {
		static constexpr U32 kSourceBits = 4;
		static constexpr U32 kSourceMask = (1u << kSourceBits) - 1;
		static constexpr U32 kMaxLanes = 4; // a 16-bit selector holds four sources

		ShuffleNode(Function& fn, Type* vecType, Node* first, Node* second, U16 sel);

		Node* getFirst() const;
		Node* getSecond() const;
		U16 getSelector() const;
		U32 getSource(U32 lane) const;
	private:
		U16 sel;
	};

	// address of a module-level symbol (global or function), as a pointer
	struct GlobalNode : FixedOpcodeNode<Opcode::Global> {
		GlobalNode(Function& fn, Type* ptrType, String symbol);

		const String& getSymbol() const;
	private:
		String symbol;
	};

	// fixed-size frame slot yielding a pointer
	struct AllocNode : FixedOpcodeNode<Opcode::Alloc> {
		AllocNode(Function& fn, Type* ptrType, Type* allocType, U32 align = 0);

		Type* getAllocType() const;
		U32 getAlign() const;
	private:
		Type* allocType;
		U32 align;
	};

	// run-time stack bump
	struct StackAllocNode : FixedOpcodeNode<Opcode::StackAlloc, AnchoredNode> {
		StackAllocNode(Function& fn, Type* ptrType, Node* control, Node* memory, Node* size);

		Node* getSize() const;
	};

	// reads the current stack top
	struct StackSaveNode : FixedOpcodeNode<Opcode::StackSave, AnchoredNode> {
		StackSaveNode(Function& fn, Type* ptrType, Node* control, Node* memory);
	};

	// puts a saved stack top back and produces the next memory state
	struct StackRestoreNode : FixedOpcodeNode<Opcode::StackRestore, AnchoredNode> {
		StackRestoreNode(Function& fn, Type* memoryType, Node* control, Node* memory, Node* saved);

		Node* getSaved() const;
	};

	// clang-format off
	// isa/dyn_cast are null-safe
	// cast is an unchecked static_cast and must only be used when the opcode is already known
	template <typename T> B32 isa(const Node* n) { return n && T::hasOpcode(n->getOpcode()); }
	template <typename T> T* cast(Node* n) { return static_cast<T*>(n); }
	template <typename T> const T* cast(const Node* n) { return static_cast<const T*>(n); }
	template <typename T> T* dyn_cast(Node* n) { return isa<T>(n) ? static_cast<T*>(n) : nullptr; }
	template <typename T> const T* dyn_cast(const Node* n) { return isa<T>(n) ? static_cast<const T*>(n) : nullptr; }
	// clang-format on

	template <typename T> List<T*> usersOfType(const Node* n) {
		List<T*> out;
		for(Node* u : n->getUsers())
			if(T* t = dyn_cast<T>(u))
				out.push_back(t);
		return out;
	}

	B32 isControlNode(const Node* n);
	// the state a load, store, call, asm or stack op reads, else null
	Node* memoryInput(const Node* n);
	// same kind and payload with null inputs, null for Start, Stop and Return
	Node* cloneShell(Function& into, const Node* n);
	// a Binary, Unary, Compare, Convert or Select node of op on its inputs, null for other ops
	Node* createArith(Function& fn, Opcode op, Type* type, Node* const* inputs);
} // namespace rat

#endif
