#include "IR/Type.h"

#include <sstream>

namespace rat {
	Type::Type(Kind kind, U32 bits, List<Type*> elements)
			: kind(kind), bits(bits), elements(std::move(elements)) {}

	B32 Type::isControl() const { return kind == Control; }
	B32 Type::isMemory() const { return kind == Memory; }
	B32 Type::isInt() const { return kind == Int; }
	B32 Type::isPtr() const { return kind == Ptr; }
	B32 Type::isTuple() const { return kind == Tuple; }
	B32 Type::isData() const { return isInt() || isPtr(); }

	U32 Type::getIntWidth() const { return bits; }

	const List<Type*>& Type::getTupleElements() const { return elements; }

	Type* Type::getTupleElement(U32 index) const { return elements[index]; }

	U32 Type::getTupleElementCount() const { return (U32)elements.size(); }

	void Type::print(std::ostream& os) const {
		switch (kind) {
		case Control:
			os << "ctrl";
			return;
		case Memory:
			os << "mem";
			return;
		case Int:
			os << 'i' << bits;
			return;
		case Ptr:
			os << "ptr";
			return;
		case Tuple:
			os << '(';
			for (U32 i = 0; i < elements.size(); ++i) {
				if (i)
					os << ", ";
				elements[i]->print(os);
			}
			os << ')';
			return;
		}
	}

	String Type::str() const {
		std::ostringstream os;
		print(os);
		return os.str();
	}

	TypeContext::TypeContext() {
		control.reset(new Type(Type::Control, 0, {}));
		memory.reset(new Type(Type::Memory, 0, {}));
		ptr.reset(new Type(Type::Ptr, 0, {}));
	}

	Type* TypeContext::getControl() { return control.get(); }
	Type* TypeContext::getMemory() { return memory.get(); }
	Type* TypeContext::getPtr() { return ptr.get(); }

	Type* TypeContext::getBool() { return getInt(1); }

	Type* TypeContext::getInt(U32 bits) {
		auto it = ints.find(bits);
		if (it != ints.end())
			return it->second.get();
		Type* t = new Type(Type::Int, bits, {});
		ints.emplace(bits, UniquePtr<Type>(t));
		return t;
	}

	Type* TypeContext::getTuple(const List<Type*>& elements) {
		for (auto& existing : tuples) {
			if (existing->getTupleElements() == elements)
				return existing.get();
		}

		Type* t = new Type(Type::Tuple, 0, elements);
		tuples.emplace_back(t);
		return t;
	}
} // namespace rat
