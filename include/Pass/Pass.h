#ifndef RAT_PASS_PASS_H
#define RAT_PASS_PASS_H

#include "Core.h"

namespace rat {
	struct Function;
	struct Module;

	struct Pass {
		virtual ~Pass();

		virtual const C8* name() const = 0;
		virtual B32 run(Module& module) = 0;
	};

	struct FunctionPass : Pass {
		B32 run(Module& module) final;
		virtual U32 runOnFunction(Function& fn) = 0;
	};
} // namespace rat

#endif
