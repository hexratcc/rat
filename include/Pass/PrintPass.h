#ifndef RAT_PASS_PRINTPASS_H
#define RAT_PASS_PRINTPASS_H

#include "Core.h"
#include "Pass/Pass.h"

#include <ostream>

namespace rat {
	struct Module;

	struct PrintPass : Pass {
		explicit PrintPass(std::ostream& os);

		const char* name() const override;
		B32 run(Module& module) override;

	private:
		std::ostream* os;
	};
} // namespace rat

#endif
