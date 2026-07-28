#ifndef RAT_CC_BUILTINHEADERS_H
#define RAT_CC_BUILTINHEADERS_H

#include "Core.h"

namespace rat::cc {
	const char* builtinIncludeDir();
	B32 readBuiltinHeader(const String& path, String& out);
} // namespace rat::cc

#endif
