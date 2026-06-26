#include "Target/Target.h"

#include "CodeGen/RegAlloc.h"

namespace rat {
	RegAllocHooks TargetInfo::regAllocHooks() const { return {}; }
} // namespace rat
