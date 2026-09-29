#ifndef RAT_CC_BUILTINHEADERS_H
#define RAT_CC_BUILTINHEADERS_H

#include <filesystem>

#include "core.h"

namespace rat::cc {
	namespace detail {
		// absolute path of the running executable, cross-platform
		std::filesystem::path selfExePath();
		String includeDirOf(const std::filesystem::path& exe);
	} // namespace detail

	const String& builtinIncludeDir();
} // namespace rat::cc

#endif
