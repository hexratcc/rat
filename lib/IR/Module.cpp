#include "IR/Module.h"

#include "CodeGen/Target.h"

namespace rat {
	Global::Global(String name, Type* type, B32 isConst, List<U8> init)
			: name(std::move(name)), type(type), isConst(isConst),
				init(std::move(init)) {}

	const String& Global::getName() const { return name; }
	Type* Global::getType() const { return type; }
	B32 Global::isConstant() const { return isConst; }
	const List<U8>& Global::getInit() const { return init; }

	Module::Module(String name) : name(std::move(name)) {}

	const String& Module::getName() const { return name; }

	const TargetInfo* Module::target() const { return tgt; }
	void Module::setTarget(const TargetInfo* t) { tgt = t; }

	U32 Module::pointerBytes() const {
		return tgt ? tgt->getPointerSizeInBytes() : 0;
	}

	Function* Module::createFunction(const String& name,
																	 const List<Type*>& params, Type* ret) {
		Function* fn = arena.make<Function>(*this, name, params, ret);
		funcs.push_back(fn);
		return fn;
	}

	Global* Module::createGlobal(const String& name, Type* type, B32 isConst,
															 List<U8> init) {
		Global* g = arena.make<Global>(name, type, isConst, std::move(init));
		globs.push_back(g);
		return g;
	}

	Global* Module::createString(const String& name, const String& bytes) {
		List<U8> init(bytes.begin(), bytes.end());
		Type* type = getArray(getInt(8), (U32)init.size());
		return createGlobal(name, type, true, std::move(init));
	}

	Global* Module::getGlobal(const String& name) const {
		for (Global* g : globs) {
			if (g->getName() == name)
				return g;
		}
		return nullptr;
	}

	const List<Global*>& Module::globals() const { return globs; }

	Function* Module::FunctionIterator::operator*() const { return *it; }

	Module::FunctionIterator& Module::FunctionIterator::operator++() {
		++it;
		return *this;
	}

	B32 Module::FunctionIterator::operator!=(
			const FunctionIterator& other) const {
		return it != other.it;
	}

	Module::FunctionIterator Module::begin() const { return {funcs.begin()}; }
	Module::FunctionIterator Module::end() const { return {funcs.end()}; }
	B32 Module::empty() const { return funcs.empty(); }
} // namespace rat
