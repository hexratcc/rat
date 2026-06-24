#ifndef RAT_SUPPORT_STRINGUTIL_H
#define RAT_SUPPORT_STRINGUTIL_H

#include "Core.h"

namespace rat {
	inline B32 readAll(std::istream& in, String& out) {
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return (B32)!in.bad();
	}

	inline String stripAnsi(const String& s) {
		String out;
		out.reserve(s.size());
		for (U32 i = 0; i < s.size();) {
			if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
				i += 2;
				while (i < s.size() && s[i] != 'm')
					++i;
				if (i < s.size())
					++i;
			} else {
				out.push_back(s[i++]);
			}
		}
		return out;
	}
} // namespace rat

#endif
