#ifndef RAT_CC_HOST_H
#define RAT_CC_HOST_H

#include "Core.h"

#include "Target/Target.h"

namespace rat::cc {
	const TargetTriple& hostTargetTriple();
	void setHostTargetTriple(const TargetTriple& triple);
} // namespace rat::cc

#endif
