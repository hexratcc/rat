#ifndef RAT_IR_TYPE_H
#define RAT_IR_TYPE_H

#include "Core.h"

#include <ostream>

namespace rat {
	struct Type {
		enum Kind { Control, Memory, Int, Ptr, Tuple };

		Type(Kind kind, U32 bits, List<Type*> elements);

		B32 isControl() const;
		B32 isMemory() const;
		B32 isInt() const;
		B32 isPtr() const;
		B32 isTuple() const;
		B32 isData() const;

		U32 getIntWidth() const;
		const List<Type*>& getTupleElements() const;
		Type* getTupleElement(U32 index) const;
		U32 getTupleElementCount() const;

		void print(std::ostream& os) const;
		String str() const;

	private:
		Kind kind;
		U32 bits;
		List<Type*> elements;
	};

	struct TypeContext {
		TypeContext();

		Type* getControl();
		Type* getMemory();
		Type* getPtr();
		Type* getInt(U32 bits);
		Type* getBool();
		Type* getTuple(const List<Type*>& elements);

	private:
		UniquePtr<Type> control;
		UniquePtr<Type> memory;
		UniquePtr<Type> ptr;
		List<UniquePtr<Type>> tuples;
		Map<U32, UniquePtr<Type>> ints;
	};
} // namespace rat

#endif
