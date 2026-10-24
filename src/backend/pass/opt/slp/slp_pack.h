// SLP vectorization: windows of stores in store chains and sets, and integer reductions, pack
// into SSE vectors. The internals are in slp_pack_detail.h

#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "core.h"

#include "pass/pass.h"

#include <iosfwd>

namespace rat {
	struct SlpStats {
		U32 windowsSeen = 0;		 // windows, and reductions of kMinTerms or more terms, examined
		U32 packedUnguarded = 0; // windows fused statically
		U32 packedGuarded = 0;	 // windows fused behind a runtime guard
		U32 packedReduction = 0; // reductions packed into vectors
		U32 guardedRuns = 0;		 // merged guard regions emitted
		U32 guardChecks = 0;		 // runtime disjointness checks emitted
		U32 rejected = 0;				 // windows rejected early, and seeds that did not pack
	};

	// the pass variants: slp, slp-noguardcost prices guards at 0 (a fuzz test mode), slp-noguards
	// rejects each window with an inner reader, so no run needs a guard (an ablation)
	enum class SlpGuards : U8 { On, NoCost, Off };

	struct SlpPackPass : FunctionPass {
		explicit SlpPackPass(SlpGuards guards = SlpGuards::On);
		const C8* name() const override;
		void beginModule(Module& module, const CallGraph& graph) override;
		void printStats(const Module& module, std::ostream& os) const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		SlpGuards guards;
		SlpStats stats;
	};
} // namespace rat

#endif
