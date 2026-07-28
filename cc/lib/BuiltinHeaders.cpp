#include "BuiltinHeaders.h"

#include <string_view>

namespace rat::cc {
	namespace {
		struct HeaderEntry {
			const char* name; // path relative to the virtual dir
			const char* text;
		};

		const HeaderEntry kHeaders[] = {
#include "BuiltinHeaders.inc"
		};

		constexpr std::string_view kDir = "<builtin>";
	} // namespace

	const char* builtinIncludeDir() { return kDir.data(); }

	B32 readBuiltinHeader(const String& path, String& out) {
		std::string_view p = path;
		if(p.size() <= kDir.size() + 1 || p.substr(0, kDir.size()) != kDir || p[kDir.size()] != '/')
			return false;
		std::string_view name = p.substr(kDir.size() + 1);
		for(const HeaderEntry& h : kHeaders) {
			if(name == h.name) {
				out = h.text;
				return true;
			}
		}
		return false;
	}
} // namespace rat::cc
