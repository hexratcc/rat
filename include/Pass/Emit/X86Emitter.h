#ifndef RAT_PASS_EMIT_X86EMITTER_H
#define RAT_PASS_EMIT_X86EMITTER_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Module;

	void emitX86(const Module& module, std::ostream& os);

	struct X86EmitterPass : Pass {
		explicit X86EmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;

	private:
		std::ostream* os;
	};
} // namespace rat

#endif
