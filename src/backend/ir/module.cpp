#include "ir/module.h"

namespace rat {
	Global::Global(String name, Type* type, B32 isConst, List<U8> init, List<Reloc> relocs)
	: name(std::move(name)),
		type(type),
		isConst(isConst),
		init(std::move(init)),
		relocs(std::move(relocs)) {}

	const String& Global::getName() const { return name; }
	Type* Global::getType() const { return type; }
	B32 Global::isConstant() const { return isConst; }
	const List<U8>& Global::getInit() const { return init; }
	const List<Reloc>& Global::getRelocs() const { return relocs; }

	Module::Module(String name)
	: name(std::move(name)) {}

	const String& Module::getName() const { return name; }

	void Module::rebuildFuncIndex() const {
		funcIndex.clear();
		for(Function* fn : funcs)
			funcIndex.emplace(fn->getName(), fn);
		funcIndexValid = true;
	}

	void Module::rebuildGlobIndex() const {
		globIndex.clear();
		for(Global* g : globs)
			globIndex.emplace(g->getName(), g);
		globIndexValid = true;
	}

	Function* Module::createFunction(const String& name, const List<Type*>& params, Type* ret) {
		Function* fn = arena.make<Function>(*this, name, params, ret);
		funcs.push_back(fn);
		if(funcIndexValid)
			funcIndex.emplace(name, fn);
		return fn;
	}

	Function* Module::getFunction(const String& name) const {
		if(!funcIndexValid)
			rebuildFuncIndex();
		auto it = funcIndex.find(name);
		if(it != funcIndex.end() && it->second->getName() == name)
			return it->second;
		for(Function* fn : funcs) {
			if(fn->getName() == name) {
				rebuildFuncIndex();
				return fn;
			}
		}
		return nullptr;
	}

	B32 Module::removeFunction(Function* fn) {
		for(auto it = funcs.begin(); it != funcs.end(); ++it) {
			if(*it == fn) {
				funcs.erase(it);
				funcIndexValid = false;
				return true;
			}
		}
		return false;
	}

	Global* Module::createGlobal(
			const String& name, Type* type, B32 isConst, List<U8> init, List<Reloc> relocs) {
		if(type && type->byteSize(8) > 0) {
			while(!init.empty() && init.back() == 0)
				init.pop_back();
			init.shrink_to_fit();
		}
		Global* g = arena.make<Global>(name, type, isConst, std::move(init), std::move(relocs));
		globs.push_back(g);
		if(globIndexValid)
			globIndex.emplace(name, g);
		return g;
	}

	Global* Module::createAlias(const String& name, const String& target, Type* type) {
		Global* g = arena.make<Global>(name, type, false, List<U8>(), List<Reloc>());
		g->setAliasTarget(target);
		globs.push_back(g);
		if(globIndexValid)
			globIndex.emplace(name, g);
		return g;
	}

	Global* Module::getGlobal(const String& name) const {
		if(!globIndexValid)
			rebuildGlobIndex();
		auto it = globIndex.find(name);
		if(it == globIndex.end())
			return nullptr;
		return it->second;
	}

	const List<Global*>& Module::globals() const { return globs; }
} // namespace rat
