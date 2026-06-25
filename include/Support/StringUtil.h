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

	inline String ltrim(const String& s) {
		U32 i = 0;
		while(i < s.size() && std::isspace((U8)s[i]))
			++i;
		return s.substr(i);
	}

	inline String rtrim(const String& s) {
		U32 e = (U32)s.size();
		while(e > 0 && std::isspace((U8)s[e - 1]))
			--e;
		return s.substr(0, e);
	}

	inline String trim(const String& s) { return rtrim(ltrim(s)); }

	inline String stripAnsi(const String& s) {
		String out;
		out.reserve(s.size());
		for(U32 i = 0; i < s.size();) {
			if(s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
				i += 2;
				while(i < s.size() && s[i] != 'm')
					++i;
				if(i < s.size())
					++i;
			} else {
				out.push_back(s[i++]);
			}
		}
		return out;
	}
} // namespace rat

#endif
