// pass entry / orchestration

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/module.h"
#include "pass/opt/alias_analysis.h"
#include "target/target.h"

#include <iostream>

namespace rat {
	using namespace slp;

	B32 SlpPackPass::statsEnabled() {
		static B32 v = envFlag("RAT_SLP_STATS");
		return v;
	}
	B32 SlpPackPass::shapesDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_SHAPES");
		return v;
	}
	B32 SlpPackPass::guardsDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARDS");
		return v;
	}
	B32 SlpPackPass::guardCostDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARD_COST");
		return v;
	}
	B32 SlpPackPass::fpReduceEnabled() {
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
								<< s.rejectedGuarded << ", overlap " << s.rejectedOverlap << ")\n"
								<< "slp[" << module.getName() << "]: nodes: wload " << s.packWideLoad << " vbin "
								<< s.packBinary << " splat " << s.packSplat << " (grouped " << s.splatGrouped
								<< ") const " << s.packConst << " frontier " << s.packFrontier << " | orient-swaps "
								<< s.orientSwaps << " memo-hits " << s.memoHits << "\n";
		}
		return changed;
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		U32 ptrBytes = target.getPointerSizeInBytes();
		AliasAnalysis aa(fn, ptrBytes);
		return Slp(fn, aa, ptrBytes, target.hasSse41(), stats).run();
	}
} // namespace rat
