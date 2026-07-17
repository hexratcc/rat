// dead function elimination: drop internal ('static') functions that nothing
// in the module refers to. A function with external linkage is visible to
// other translation units and is always kept; an internal function is removed
// once it has no direct callers and its address is not taken anywhere. Removals
// are iterated to a fixpoint so a chain of internal helpers that only call one
// another falls away together.

#ifndef RAT_PASS_OPT_DEADFUNCELIM_H
#define RAT_PASS_OPT_DEADFUNCELIM_H

#include "Core.h"
#include "Support/Pass.h"

namespace rat {
	struct Function;
	struct Module;

	struct DeadFuncElimPass : Pass {
		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;
	private:
		void collectReferenced(Function& fn, Set<String>& referenced);
	};
} // namespace rat

#endif
