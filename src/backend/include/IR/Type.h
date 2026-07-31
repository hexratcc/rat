#ifndef RAT_IR_TYPE_H
#define RAT_IR_TYPE_H

#include "Core.h"

namespace rat {
	struct Type {
		enum Kind {
			Control, // For control tokens
			Memory,	 // For control tokens
			Int,
			Float,
			Ptr,
			Tuple, //  multi-result producers (Start/If/Call), reached via Proj
			Array
		};

		Type(Kind kind, U32 bits, List<Type*> elements);
		U32 getUid() const;

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
		friend struct TypeContext;

		Kind kind;
		U32 bits;
		U32 uid = 0;
		List<Type*> elements;
	};

	// interning context for types
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
		Type* make(Type::Kind kind, U32 bits, List<Type*> elements);

		U32 nextUid = 0;
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
