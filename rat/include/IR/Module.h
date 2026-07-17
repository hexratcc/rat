#ifndef RAT_IR_MODULE_H
#define RAT_IR_MODULE_H

#include "Core.h"
#include "IR/Function.h"
#include "IR/Type.h"

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

		enum class Linkage { External, Internal };
		Linkage getLinkage() const { return linkage; }
		void setLinkage(Linkage l) { linkage = l; }
		B32 isInternal() const { return linkage == Linkage::Internal; }
	private:
		String name;
		Type* type;
		B32 isConst;
		List<U8> init;
		List<Reloc> relocs;
		Linkage linkage = Linkage::External;
	};

	struct Module : TypeContext {
		explicit Module(String name = "module");

		const String& getName() const;

		Function* createFunction(const String& name, const List<Type*>& params, Type* ret);
		Function* getFunction(const String& name) const;
		B32 removeFunction(Function* fn);

		Global* createGlobal(
				const String& name, Type* type, B32 isConst, List<U8> init, List<Reloc> relocs = {});
		Global* getGlobal(const String& name) const;

		const List<Global*>& globals() const;

		struct FunctionIterator {
			Function* operator*() const;
			FunctionIterator& operator++();
			B32 operator!=(const FunctionIterator& other) const;

			List<Function*>::const_iterator it;
		};

		FunctionIterator begin() const;
		FunctionIterator end() const;
	private:
		String name;
		List<Function*> funcs;
		List<Global*> globs;
	};
} // namespace rat

#endif
