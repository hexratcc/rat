#include "IR/Module.h"

namespace rat {
	Module::Module(String name) : name(std::move(name)) {}

	const String& Module::getName() const { return name; }

	const TargetInfo* Module::target() const { return tgt; }
	void Module::setTarget(const TargetInfo* t) { tgt = t; }

	Function* Module::createFunction(const String& name,
																	 const List<Type*>& params, Type* ret) {
		Function* fn = arena.make<Function>(*this, name, params, ret);
		funcs.push_back(fn);
		return fn;
	}

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
