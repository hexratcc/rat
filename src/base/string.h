#ifndef RAT_BASE_STRING_H
#define RAT_BASE_STRING_H

#include "core.h"

#include <cctype>
#include <sstream>

namespace rat {
	inline B32 readAll(std::istream& in, String& out) {
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return (B32)!in.bad();
	}

	constexpr const C8* kSpaces = " \t\n\v\f\r";

	inline String ltrim(const String& s) {
		U64 first = s.find_first_not_of(kSpaces);
		return first == String::npos ? String() : s.substr(first);
	}

	inline String trim(const String& s) {
		String t = ltrim(s);
		return t.substr(0, t.find_last_not_of(kSpaces) + 1);
	}

	// comma/whitespace separated tokens
	inline List<String> splitTokens(String s) {
		for(C8& c : s)
			if(c == ',')
				c = ' ';
		std::istringstream ss(s);
		List<String> out;
		for(String t; ss >> t;)
			out.push_back(t);
		return out;
	}

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
