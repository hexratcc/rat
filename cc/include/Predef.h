#ifndef RAT_CC_PREDEF_H
#define RAT_CC_PREDEF_H

#include "Core.h"

#include "Target/Target.h"

namespace rat::cc {
	const String& builtinPredefs(const TargetTriple& triple);
} // namespace rat::cc

#endif
