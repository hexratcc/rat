#ifndef RAT_PASS_EMIT_TEXTEMITTER_H
#define RAT_PASS_EMIT_TEXTEMITTER_H

#include "Core.h"
#include "Pass/Pass.h"

#include <ostream>

namespace rat {
	struct Function;
	struct Module;

	void emitText(const Function& fn, std::ostream& os);
	void emitText(const Module& module, std::ostream& os);

	struct TextEmitterPass : Pass {
		explicit TextEmitterPass(std::ostream& os);

		const char* name() const override;
		B32 run(Module& module) override;

	private:
		std::ostream* os;
	};
} // namespace rat

#endif
