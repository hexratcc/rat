#ifndef RAT_PASS_EMIT_GRAPHEMITTER_H
#define RAT_PASS_EMIT_GRAPHEMITTER_H

#include "core.h"
#include "pass/pass.h"

namespace rat {
	struct GraphEmitterPass : Pass {
		explicit GraphEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
