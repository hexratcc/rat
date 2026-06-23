#ifndef RAT_IR_FUNCTION_H
#define RAT_IR_FUNCTION_H

#include "Core.h"
#include "IR/Node.h"
#include "IR/Type.h"

#include <functional>

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

		B32 isVariadic() const { return variadic; }
		void setVariadic(B32 v) { variadic = v; }

		StartNode* getStart() const;
		StopNode* getStop() const;

		template <typename T, typename... Args> T* create(Args&&... args) {
			T* node = arena.make<T>(*this, std::forward<Args>(args)...);
			nodes.push_back(node);
			return node;
		}

		struct Block;
		using Var = U32;

		Type* boolTy() const;
		Type* intTy(U32 bits) const;
		Type* ptrTy() const;
		Type* memTy() const;
		Type* ctrlTy() const;
		Node* control() const;
		Node* memory();
		Node* param(U32 index);

		Node* constInt(Type* type, I64 value);
		Node* constBool(B32 value);
		Node* constFloat(Type* type, double value);

		Node* binary(Opcode op, Node* lhs, Node* rhs);
		Node* add(Node* lhs, Node* rhs);
		Node* sub(Node* lhs, Node* rhs);
		Node* mul(Node* lhs, Node* rhs);
		Node* sdiv(Node* lhs, Node* rhs);
		Node* udiv(Node* lhs, Node* rhs);
		Node* srem(Node* lhs, Node* rhs);
		Node* urem(Node* lhs, Node* rhs);
		Node* and_(Node* lhs, Node* rhs);
		Node* or_(Node* lhs, Node* rhs);
		Node* xor_(Node* lhs, Node* rhs);
		Node* shl(Node* lhs, Node* rhs);
		Node* lshr(Node* lhs, Node* rhs);
		Node* ashr(Node* lhs, Node* rhs);

		Node* unary(Opcode op, Node* operand);
		Node* neg(Node* operand);
		Node* bitNot(Node* operand);

		Node* compare(Opcode op, Node* lhs, Node* rhs);
		Node* eq(Node* lhs, Node* rhs);
		Node* ne(Node* lhs, Node* rhs);
		Node* slt(Node* lhs, Node* rhs);
		Node* sle(Node* lhs, Node* rhs);
		Node* sgt(Node* lhs, Node* rhs);
		Node* sge(Node* lhs, Node* rhs);
		Node* ult(Node* lhs, Node* rhs);
		Node* ule(Node* lhs, Node* rhs);
		Node* ugt(Node* lhs, Node* rhs);
		Node* uge(Node* lhs, Node* rhs);

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

		IfNode* iff(Node* predicate);
		ProjNode* proj(Node* tuple, U32 index, Type* type, String label = "");
		RegionNode* region(const List<Node*>& preds);
		PhiNode* phi(Type* type, RegionNode* region, const List<Node*>& values);

		Block* createBlock(String name = "");
		Block* createLoopHeader(String name = "");
		void setInsertBlock(Block* block);
		Block* insertBlock() const;
		B32 blockFinished() const;
		void seal(Block* block);
		void jmp(Block* target);
		void jumpif(Node* cond, Block* target);

		// locals: declare with newVar (define later) or declareLocal (with an
		// initializer), then get / set. SSA phi placement is handled for you.
		Var newVar(String name, Type* type);
		Var declareLocal(String name, Node* init);
		Node* get(Var var);
		void set(Var var, Node* value);
		const String& localName(Var var) const;
		U32 numLocals() const;

		void loop(const std::function<void()>& bodyFn);
		void break_();
		void continue_();
		void breakIf(Node* cond);
		void continueIf(Node* cond);

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

		U32 eliminateDeadNodes(B32 includeControl = false);
		void removeNode(Node* n);

		friend struct Node;

	private:
		U32 allocateId();

		void writeVar(Var var, Node* value);
		Node* readVar(Var var);

		Node* readVariable(Var var, Block* block);
		Node* readVariableRecursive(Var var, Block* block);
		Node* addPhiOperands(Var var, PhiNode* phi, Block* block);
		Node* tryRemoveTrivialPhi(PhiNode* phi);
		PhiNode* newIncompletePhi(Var var, Block* block);
		void replacePhiEverywhere(PhiNode* phi, Node* with);

		Block* makeBlock(String name, B32 loopHeader);
		void addEdge(Node* exitControl, Block* from, Block* to);
		void activateOnSeal(Block* block);

		Module* mod;
		String name;
		List<Type*> paramTypes;
		Type* retType; // null for a void function
		B32 variadic = false;

		Arena arena;
		List<Node*> nodes; // in creation order
		U32 nextId = 0;

		StartNode* start = nullptr;
		StopNode* stop = nullptr;

		List<Block*> blocks; // every block
		Block* cur = nullptr; // current insertion block

		struct VarInfo {
			String name;
			Type* ty;
		};
		List<VarInfo> varInfos;
		Var memVar = 0; // reserved variable carrying the memory token

		struct LoopCtx {
			Block* header;
			Block* exit;
		};
		List<LoopCtx> loopStack;

		List<Node*> paramCache;
	};
} // namespace rat

#endif
