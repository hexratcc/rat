#include "Pass/Opt/SlpPack.h"

namespace rat {
	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		(void)fn;
		(void)target;
		return 0;
	}
} // namespace rat
