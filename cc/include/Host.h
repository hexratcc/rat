#ifndef RAT_CC_HOST_H
#define RAT_CC_HOST_H

#include "Core.h"
#include <cstdio>

#include "Target/Target.h"

namespace rat::cc {
	const TargetTriple& hostTargetTriple();
	void setHostTargetTriple(const TargetTriple& triple);

	const char* nullDevice();
	FILE* shellOpen(const char* cmd);
	I32 shellClose(FILE* p);

	const String& hostCC();
	const String& hostPredefs();
	const List<String>& hostIncludeDirs();
} // namespace rat::cc

#endif
