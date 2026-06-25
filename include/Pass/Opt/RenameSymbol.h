#ifndef RAT_PASS_OPT_RENAMESYMBOL_H
#define RAT_PASS_OPT_RENAMESYMBOL_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Module;

	struct RenameSymbolPass : Pass {
		RenameSymbolPass(String from, String to);

		const C8* name() const override;
		B32 run(Module& module) override;
	private:
		String from;
		String to;
	};
} // namespace rat

#endif
