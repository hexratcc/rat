#ifndef RAT_IR_TYPE_H
#define RAT_IR_TYPE_H

#include "Core.h"

namespace rat {
	struct Type {
		enum Kind { Control, Memory, Int, Float, Ptr, Tuple, Array };

		Type(Kind kind, U32 bits, List<Type*> elements);

		B32 isControl() const;
		B32 isMemory() const;
		B32 isInt() const;
		B32 isFloat() const;
		B32 isPtr() const;
		B32 isTuple() const;
		B32 isArray() const;
		B32 isData() const;

		U32 getIntWidth() const;
		U32 getFloatWidth() const;
		const List<Type*>& getTupleElements() const;
		Type* getTupleElement(U32 index) const;
		U32 getTupleElementCount() const;

		Type* getArrayElement() const;
		U32 getArrayCount() const;
		U32 byteSize(U32 ptrBytes) const;

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
		Type* getFloat(U32 bits);
		Type* getBool();
		Type* getTuple(const List<Type*>& elements);
		Type* getArray(Type* element, U32 count);
	protected:
		Arena arena;
	private:
		Type* control = nullptr;
		Type* memory = nullptr;
		Type* ptr = nullptr;
		List<Type*> tuples;
		List<Type*> arrays;
		Map<U32, Type*> ints;
		Map<U32, Type*> floats;
	};
} // namespace rat

#endif
