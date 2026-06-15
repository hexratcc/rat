#ifndef RAT_PASS_PASS_H
#define RAT_PASS_PASS_H

#include "Core.h"

namespace rat {
	struct Module;

	struct Pass {
		virtual ~Pass();

		virtual const char* name() const = 0;
		virtual B32 run(Module& module) = 0;
	};
} // namespace rat

#endif
