#ifndef RAT_IR_MODULE_H
#define RAT_IR_MODULE_H

#include "Core.h"
#include "IR/Function.h"
#include "IR/Type.h"

namespace rat {
	struct TargetInfo;

	struct Global {
		Global(String name, Type* type, B32 isConst, List<U8> init);

		const String& getName() const;
		Type* getType() const;
		B32 isConstant() const;
		const List<U8>& getInit() const;

	private:
		String name;
		Type* type;
		B32 isConst;
		List<U8> init;
	};

	struct Module : TypeContext {
		explicit Module(String name = "module");

		const String& getName() const;

		const TargetInfo* target() const;
		void setTarget(const TargetInfo* t);

		U32 pointerBytes() const;

		Function* createFunction(const String& name, const List<Type*>& params,
														 Type* ret);

		Global* createGlobal(const String& name, Type* type, B32 isConst,
												 List<U8> init);
		Global* createString(const String& name, const String& bytes);
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
		B32 empty() const;

	private:
		String name;
		const TargetInfo* tgt = nullptr;
		List<Function*> funcs;
		List<Global*> globs;
	};
} // namespace rat

#endif
