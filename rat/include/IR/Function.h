// a function owns its nodes and exposes two layers of API:
// - graph layer:    create<T>() for raw node construction, iteration over all nodes, and
//                   maintenance helpers (ie. for passes)
// - builder layer:  blocks, jumps, and named variables, so a frontend can emit straight-line code
//                   statement by statement and get a valid graph with phis already placed
// emission always happens into the current insertion block (setInsertBlock). a block collects its
// predecessor control edges as other blocks jump to it and must be sealed once no further
// predecessors can appear.
#ifndef RAT_IR_FUNCTION_H
#define RAT_IR_FUNCTION_H

#include "Core.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	struct Module;

	struct Function {
		Function(Module& module, String name, const List<Type*>& params, Type* ret);
		~Function();

		Module& getModule() const;
		TypeContext& types() const;
		const String& getName() const;
		void setName(String n);

		U32 getParamCount() const;
		Type* getParamType(U32 index) const;
		Type* getReturnType() const;
		B32 returnsValue() const;

		B32 isVariadic() const;
		void setVariadic(B32 v);

		// symbol linkage
		enum class Linkage { External, Internal };
		Linkage getLinkage() const { return linkage; }
		void setLinkage(Linkage l) { linkage = l; }
		B32 isInternal() const { return linkage == Linkage::Internal; }

		StartNode* getStart() const;
		StopNode* getStop() const;

		template <typename T, typename... Args> T* create(Args&&... args) {
			T* node = arena.make<T>(*this, std::forward<Args>(args)...);
			nodes.push_back(node);
			return node;
		}

		// builder until seal() fixes the predecessor set
		struct Block {
			RegionNode* region = nullptr;
			List<Node*> preds;			 // incoming control edges
			List<Block*> predBlocks; // source block per pred (parallel)
			Node* ctrl = nullptr;		 // control anchor once active
			B32 sealed = false;
			B32 active = false; // ctrl established
			B32 loopHeader = false;
			B32 finished = false; // ended in a terminator
			Map<U32, Node*> defs; // cache current SSA value of each Var
			Map<U32, PhiNode*> incompletePhis;
		};

		using Var = U32;

		// types
		Type* boolTy() const;
		Type* ptrTy() const;
		Type* memTy() const;
		Type* ctrlTy() const;
		Node* control() const;
		Node* memory();
		Node* param(U32 index);

		Node* constInt(Type* type, I64 value);
		Node* constBool(B32 value);
		Node* constFloat(Type* type, F64 value);

		// operations
		Node* binary(Opcode op, Node* lhs, Node* rhs);
		Node* add(Node* lhs, Node* rhs);
		Node* sub(Node* lhs, Node* rhs);
		Node* mul(Node* lhs, Node* rhs);
		Node* sdiv(Node* lhs, Node* rhs);
		Node* and_(Node* lhs, Node* rhs);
		Node* or_(Node* lhs, Node* rhs);
		Node* shl(Node* lhs, Node* rhs);
		Node* lshr(Node* lhs, Node* rhs);
		Node* ashr(Node* lhs, Node* rhs);

		Node* unary(Opcode op, Node* operand);
		Node* neg(Node* operand);
		Node* bitNot(Node* operand);

		Node* compare(Opcode op, Node* lhs, Node* rhs);
		Node* eq(Node* lhs, Node* rhs);
		Node* ne(Node* lhs, Node* rhs);

		Node* convert(Opcode op, Node* operand, Type* destType);
		Node* trunc(Node* operand, Type* destType);
		Node* sext(Node* operand, Type* destType);
		Node* zext(Node* operand, Type* destType);

		Node* load(Type* valueType, Node* pointer);
		void store(Node* pointer, Node* value);

		Node* global(const String& name);
		Node* alloc(Type* type);
		Node* allocVLA(Type* type, Node* byteCount);

		Node* call(const String& callee, Type* retType, const List<Node*>& args);
		Node* callIndirect(Node* target, Type* retType, const List<Node*>& args);

		// control
		IfNode* iff(Node* predicate);
		ProjNode* proj(Node* tuple, U32 index, Type* type, String label = "");
		RegionNode* region(const List<Node*>& preds);
		PhiNode* phi(Type* type, RegionNode* region, const List<Node*>& values);

		// block api
		Block* createBlock(String name = "");

		// activate immediately so the body can be emitted before the back edge exists, and are sealed
		// after the latch is wired up
		Block* createLoopHeader(String name = "");
		void setInsertBlock(Block* block);
		B32 blockFinished() const;

		// sealing activates the block: zero preds leaves it inactive (unreachable), one pred forwards
		// that control edge directly, two or more create a region node
		void seal(Block* block);
		void jmp(Block* target);
		void jumpif(Node* cond, Block* target);

		// locals: declare with newVar (define later) or declareLocal (with an
		// initializer), then get / set. SSA phi placement is handled for you.
		Var newVar(String name, Type* type);
		Var declareLocal(String name, Node* init);
		Node* get(Var var);
		void set(Var var, Node* value);

		// terminators
		void ret(Node* value);
		void retVoid();

		struct NodeIterator {
			List<Node*>::const_iterator it;
			Node* operator*() const;
			NodeIterator& operator++();
			B32 operator!=(const NodeIterator& other) const;
		};

		NodeIterator begin() const;
		NodeIterator end() const;
		U32 size() const;
		B32 hasReturn() const;

		U32 eliminateDeadNodes(B32 includeControl = false);
		U32 pruneUnreachable();
		void removeNode(Node* n);

		friend struct Node;
	private:
		U32 allocateId();

		void writeVar(Var var, Node* value);
		Node* readVar(Var var);

		Node* readVariable(Var var, Block* block);
		Node* readVariableRecursive(Var var, Block* block);
		// tracks (block, var) slots holding a phi so trivial-phi removal patches
		// them without scanning every block
		void cacheDef(Block* block, Var var, Node* val);
		Node* addPhiOperands(Var var, PhiNode* phi, Block* block);
		Node* tryRemoveTrivialPhi(PhiNode* phi);
		PhiNode* newIncompletePhi(Var var, Block* block);
		void replacePhiEverywhere(PhiNode* phi, Node* with);
		Map<PhiNode*, List<std::pair<Block*, Var>>> phiDefSites;

		Block* makeBlock(B32 loopHeader);
		void addEdge(Node* exitControl, Block* from, Block* to);
		void activateOnSeal(Block* block);

		Type* callTupleType(Type* retType);
		Node* attachCallProjections(CallNode* c, Type* retType);

		Module* mod;
		String name;
		List<Type*> paramTypes;
		Type* retType; // null for a void function
		B32 variadic = false;
		Linkage linkage = Linkage::External;

		Arena arena;
		List<Node*> nodes; // in creation order
		U32 nextId = 0;

		StartNode* start = nullptr;
		StopNode* stop = nullptr;

		List<Block*> blocks;	// every block
		Block* cur = nullptr; // current insertion block

		struct VarInfo {
			String name;
			Type* ty;
		};
		List<VarInfo> varInfos;
		Var memVar = 0; // reserved variable carrying the memory token

		List<Node*> paramCache;
	};
} // namespace rat

#endif
