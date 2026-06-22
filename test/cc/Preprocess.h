#ifndef RAT_CC_PREPROCESS_H
#define RAT_CC_PREPROCESS_H

#include "Core.h"

namespace rat::cc {
	struct PpOptions {
		List<String> includeDirs;
		List<String> defines;
		List<String> undefs;
	};

	B32 preprocess(const String& path, const String& source,
								 const PpOptions& opts, String& out, String& err);
} // namespace rat::cc

#endif
