#ifndef RAT_PASS_EMIT_GRAPHEMITTER_H
#define RAT_PASS_EMIT_GRAPHEMITTER_H

#include "Core.h"
#include "Pass/Pass.h"

#include <ostream>

namespace rat {
	struct Function;
	struct Module;

	void emitDot(const Function& fn, std::ostream& os);
	void emitDot(const Module& module, std::ostream& os);

	struct GraphEmitterPass : Pass {
		explicit GraphEmitterPass(std::ostream& os);

		const char* name() const override;
		B32 run(Module& module) override;

	private:
		std::ostream* os;
	};
} // namespace rat

#endif
