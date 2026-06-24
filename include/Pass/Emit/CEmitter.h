#ifndef RAT_PASS_EMIT_CEMITTER_H
#define RAT_PASS_EMIT_CEMITTER_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;

	void emitC(const Function& fn, std::ostream& os);
	void emitC(const Module& module, std::ostream& os);

	struct CEmitterPass : Pass {
		explicit CEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;

	private:
		std::ostream* os;
	};
} // namespace rat

#endif
