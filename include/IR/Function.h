#ifndef RAT_IR_FUNCTION_H
#define RAT_IR_FUNCTION_H

#include "Core.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	struct Module;

	struct Function {
		Function(Module& module, String name, const List<Type*>& params, Type* ret);

		Module& getModule() const;
		TypeContext& types() const;
		const String& getName() const;

		U32 getParamCount() const;
		Type* getParamType(U32 index) const;
		Type* getReturnType() const;
		B32 returnsValue() const;

		StartNode* getStart() const;
		StopNode* getStop() const;

		template <typename T, typename... Args> T* create(Args&&... args) {
			auto owned = std::make_unique<T>(*this, std::forward<Args>(args)...);
			T* raw = owned.get();
			nodes.push_back(std::move(owned));
			return raw;
		}

		Node* param(U32 index);
		Node* constInt(Type* type, I64 value);
		Node* constBool(B32 value);
		Node* binary(Opcode op, Node* lhs, Node* rhs);
		Node* add(Node* lhs, Node* rhs);
		Node* sub(Node* lhs, Node* rhs);
		Node* mul(Node* lhs, Node* rhs);
		void ret(Node* value);
		void retVoid();

		struct NodeIterator {
			List<UniquePtr<Node>>::const_iterator it;
			Node* operator*() const;
			NodeIterator& operator++();
			B32 operator!=(const NodeIterator& other) const;
		};

		NodeIterator begin() const;
		NodeIterator end() const;
		U32 size() const;

		friend struct Node;

	private:
		U32 allocateId();

		Module* mod;
		String name;
		List<Type*> paramTypes;
		Type* retType; // null for a void function

		List<UniquePtr<Node>> nodes; // ownership of all nodes
		U32 nextId = 0;

		StartNode* start = nullptr;
		StopNode* stop = nullptr;
		ProjNode* control = nullptr; // projected control token of start
		ProjNode* memory = nullptr;	 // projected memory token of start
		List<Node*> paramCache;			 // memoized parameter projections
	};
} // namespace rat

#endif
