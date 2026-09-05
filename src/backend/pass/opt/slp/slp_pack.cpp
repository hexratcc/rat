// pass entry / orchestration

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/module.h"

#include <iostream>

namespace rat {
	using namespace slp;

	B32 slp::statsEnabled() {
		static B32 v = envFlag("RAT_SLP_STATS");
		return v;
	}
	B32 slp::shapesDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_SHAPES");
		return v;
	}
	B32 slp::guardsDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARDS");
		return v;
	}
	B32 slp::guardCostDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARD_COST");
		return v;
	}
	B32 slp::fpReduceEnabled() {
		static B32 v = envFlag("RAT_SLP_FP_REDUCE");
		return v;
	}

	const C8* SlpPackPass::name() const { return "slp"; }

	B32 SlpPackPass::run(Module& module, const TargetInfo& target) {
		stats = SlpStats{};
		B32 changed = FunctionPass::run(module, target);
		if(statsEnabled()) {
			const SlpStats& s = stats;
			std::cerr << "slp[" << module.getName() << "]: windows " << s.windowsSeen << " packed "
								<< (s.packedUnguarded + s.packedGuarded) << " (static " << s.packedUnguarded
								<< ", guarded " << s.packedGuarded << " in " << s.guardedRuns << " runs, "
								<< s.guardPairs << " checks)"
								<< " reductions " << s.packedReduction << " rejected "
								<< (s.rejectedTree + s.rejectedProfit + s.rejectedGuarded + s.rejectedOverlap)
								<< " (tree " << s.rejectedTree << ", profit " << s.rejectedProfit << ", guard "
								<< s.rejectedGuarded << ", overlap " << s.rejectedOverlap << ")\n";
		}
		return changed;
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		return Slp(fn, target, stats).run();
	}
} // namespace rat
