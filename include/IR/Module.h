#ifndef RAT_IR_MODULE_H
#define RAT_IR_MODULE_H

#include "Core.h"
#include "IR/Function.h"
#include "IR/Type.h"

namespace rat {
	struct TargetInfo;

	struct Module : TypeContext {
		explicit Module(String name = "module");

		const String& getName() const;

		const TargetInfo* target() const;
		void setTarget(const TargetInfo* t);

		Function* createFunction(const String& name, const List<Type*>& params,
														 Type* ret);

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
	};
} // namespace rat

#endif
