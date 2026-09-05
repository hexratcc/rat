#ifndef RAT_IR_MODULE_H
#define RAT_IR_MODULE_H

#include "core.h"
#include "ir/function.h"
#include "ir/type.h"

namespace rat {
	// a relocation inside a global's initializer
	struct Reloc {
		U32 offset = 0;
		String symbol;
		I64 addend = 0;
	};

	// module-level data symbol
	struct Global {
		Global(String name, Type* type, B32 isConst, List<U8> init, List<Reloc> relocs = {});

		const String& getName() const;
		Type* getType() const;
		B32 isConstant() const;
		const List<U8>& getInit() const;
		const List<Reloc>& getRelocs() const;

		using Linkage = rat::Linkage;
		void setLinkage(Linkage l) { linkage = l; }
		B32 isInternal() const { return linkage == Linkage::Internal; }

		const String& getAliasTarget() const { return aliasOf; }
		B32 isAlias() const { return !aliasOf.empty(); }
		void setAliasTarget(String target) { aliasOf = std::move(target); }

		U32 getAlign() const { return align; }
		void setAlign(U32 v) { align = v; }
	private:
		String name;
		Type* type;
		B32 isConst;
		List<U8> init;
		List<Reloc> relocs;
		Linkage linkage = Linkage::External;
		String aliasOf;
		U32 align = 0;
	};

	struct Module : TypeContext {
		explicit Module(String name = "module");

		const String& getName() const;

		Function* createFunction(const String& name, const List<Type*>& params, Type* ret);
		Function* getFunction(const String& name) const;
		B32 removeFunction(Function* fn);

		Global* createGlobal(
				const String& name, Type* type, B32 isConst, List<U8> init, List<Reloc> relocs = {});
		Global* createAlias(const String& name, const String& target, Type* type);
		Global* getGlobal(const String& name) const;

		const List<Global*>& globals() const;

		struct FunctionIterator {
			List<Function*>::const_iterator it;
			Function* operator*() const { return *it; }
			FunctionIterator& operator++() {
				++it;
				return *this;
			}
			B32 operator!=(const FunctionIterator& other) const { return it != other.it; }
		};

		FunctionIterator begin() const { return {funcs.begin()}; }
		FunctionIterator end() const { return {funcs.end()}; }
	private:
		String name;
		List<Function*> funcs;
		List<Global*> globs;

		mutable Map<String, Function*> funcIndex;
		mutable Map<String, Global*> globIndex;
		mutable B32 funcIndexValid = false;
		mutable B32 globIndexValid = false;

		void rebuildFuncIndex() const;
		void rebuildGlobIndex() const;
	};
} // namespace rat

#endif
