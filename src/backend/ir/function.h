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

#include "core.h"
#include "ir/debug_info.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	struct Module;

	// symbol linkage, shared by functions and globals
	enum class Linkage {
		External,
		Internal
	};

	constexpr U32 kMaxRestrictParams = 64;

	struct FunctionAttrs {
		Linkage linkage = Linkage::External;
		U32 align = 0; // 0 = target default
		B32 variadic = false;
		B32 noInline = false;
		U64 restrictParams = 0;
		CallAbi abi; // aggregate params and return

		B32 isInternal() const { return linkage == Linkage::Internal; }
	};

	struct Function {
		Function(Module& module, String name, const List<Type*>& params, Type* ret);

		Module& getModule() const;
		TypeContext& types() const;
		const String& getName() const;

		U32 getParamCount() const;
		Type* getParamType(U32 index) const;
		Type* getReturnType() const;
		B32 returnsValue() const;

		const FunctionAttrs& getAttrs() const;
		FunctionAttrs& getAttrs();

		// nodes created from here on carry loc
		void setLoc(U32 loc);
		DebugBody* getDebug() const;
		void setDebug(UniquePtr<DebugBody> d);

		StartNode* getStart() const;
		StopNode* getStop() const;

		template <typename T, typename... Args> T* create(Args&&... args) {
			T* node = arena.make<T>(*this, std::forward<Args>(args)...);
			nodes.push_back(node);
			return node;
		}

		using Var = U32;
		using Nodes = List<Node*>;

		// builder until seal() fixes the predecessor set
		struct Block {
			RegionNode* region = nullptr;
			List<Node*> preds;			 // incoming control edges
			List<Block*> predBlocks; // source block per pred (parallel)
			Node* ctrl = nullptr;		 // control anchor once active
			B32 sealed = false;
			B32 finished = false; // ended in a terminator
			List<Pair<Var, Node*>> defs;
			List<Pair<Var, PhiNode*>> incompletePhis;
		};

		// types
		Type* boolTy() const;
		Type* ptrTy() const;
		Type* memTy() const;
		Type* ctrlTy() const;
		Node* control() const;
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

		Node* unary(Opcode op, Node* in);
		Node* neg(Node* in);
		Node* bitNot(Node* in);
		Node* ctz(Node* in);
		Node* bswap(Node* in);

		Node* compare(Opcode op, Node* lhs, Node* rhs);
		Node* eq(Node* lhs, Node* rhs);
		Node* ne(Node* lhs, Node* rhs);

		Node* convert(Opcode op, Node* in, Type* to);
		Node* trunc(Node* in, Type* to);
		Node* sext(Node* in, Type* to);
		Node* zext(Node* in, Type* to);

		Node* load(Type* ty, Node* ptr, B32 isVolatile = false);
		void store(Node* pointer, Node* value, B32 isVolatile = false);

		Node* global(const String& name);
		Node* alloc(Type* type, U32 align = 0);

		Node* stackAlloc(Node* byteCount);
		Node* stackSave();
		void stackRestore(Node* saved);

		Node* call(const String& callee,
							 Type* retType,
							 const Nodes& args,
							 B32 varArgs = true,
							 const CallAbi& abi = {});
		Node* callIndirect(Node* target,
											 Type* retType,
											 const Nodes& args,
											 B32 varArgs = true,
											 const CallAbi& abi = {});

		List<Node*> inlineAsm(const String& text, const List<Type*>& outs, const Nodes& args);

		// control
		ProjNode* proj(Node* tuple, U32 index, Type* type, const C8* label);
		PhiNode* phi(Type* type, RegionNode* region, const Nodes& values);

		// block api
		Block* createBlock(String name = "");

		// activate immediately so the body can be emitted before the back edge exists, and are sealed
		// after the latch is wired up
		Block* createLoopHeader(String name = "");
		void setInsertBlock(Block* block);
		B32 blockFinished() const;

		// sealing activates the block, zero preds leaves it inactive (unreachable), one pred forwards
		// that control edge directly, two or more create a region node
		void seal(Block* block);
		void enterBlock(Block* block);
		void jmp(Block* target);
		void jumpif(Node* cond, Block* target);
		void switchJump(Node* selector, const List<Block*>& targets);

		// locals are declared with newVar (define later) or declareLocal (with an initializer),
		// then get and set them, SSA phi placement is handled for you
		Var newVar(String name, Type* type);
		Var declareLocal(String name, Node* init);
		Node* get(Var var);
		void set(Var var, Node* value);
		void bindVar(Var var, U32 inst);
		void bindDebug(U32 inst, Node* value);

		// terminators
		void ret(Node* value);
		void retVoid();

		List<Node*>::const_iterator begin() const { return nodes.begin(); }
		List<Node*>::const_iterator end() const { return nodes.end(); }
		U32 size() const { return (U32)nodes.size(); }
		U32 idBound() const { return nextId; }

		U64 getVersion() const { return version; }
		void touch() { ++version; }
		B32 isCleanFor(const void* pass) const;
		void markCleanFor(const void* pass);
		B32 hasReturn() const;

		U32 eliminateDeadNodes(B32 includeControl = false);
		U32 pruneUnreachable();
		void removeNode(Node* n);

		friend struct Node;
		friend struct ProjNode;
	private:
		U32 allocateId();
		Node** allocEdges(U32 count) { return arena.makeArray<Node*>(count); }
		U32* allocSlots(U32 count) { return arena.makeArray<U32>(count); }
		const C8* internString(const C8* s, U64 len) { return arena.internString(s, len); }
		B32 isDeadNode(Node* n, B32 includeControl) const;
		U32 eraseMarked(const List<U8>& mark);

		// ssa construction
		Node* mem();
		Node* read(Var var, Block* block);
		Node* readRecursive(Var var, Block* block);
		// tracks (block, var) slots holding a phi so trivial-phi removal patches
		// them without scanning every block
		void cacheDef(Block* block, Var var, Node* val);
		static Node** findDef(Block* block, Var var);
		void noteBinding(Var var, Node* value, Node* anchor, Node* after);
		Node* addPhiOperands(Var var, PhiNode* phi, Block* block);
		Node* tryRemoveTrivialPhi(PhiNode* phi);
		void replacePhiEverywhere(PhiNode* phi, Node* with);

		// blocks
		void addEdge(Node* exit, Block* to);
		void activateOnSeal(Block* block);

		// calls
		void bindEffects(Node* n);
		Node* emitCall(
				const String& sym, Node* target, const Nodes& args, Type* ret, B32 va, const CallAbi& abi);

		// signature
		Module* mod;
		String name;
		List<Type*> paramTypes;
		Type* retType; // null for a void function
		FunctionAttrs attrs;
		UniquePtr<DebugBody> debug;
		U32 curLoc = 0;

		// graph
		Arena arena;
		List<Node*> nodes; // in creation order
		U32 nextId = 0;
		StartNode* start = nullptr;
		StopNode* stop = nullptr;
		List<Node*> paramCache;

		// pass state
		U64 version = 0;					// bumped on every mutation
		U64 deadFreeAt = ~(U64)0; // version at the last dead-node sweep
		List<Pair<const void*, U64>> cleanAt;

		// builder state
		Block* cur = nullptr; // current insertion block
		List<Type*> varTypes;
		Var memVar = 0; // reserved variable carrying the memory token
		Map<PhiNode*, List<Pair<Block*, Var>>> phiDefSites;
		List<U32> varInst;					// var -> its debug var inst + 1
		Node* lastEffect = nullptr; // store or call last made, for debug bindings
	};
} // namespace rat

#endif
