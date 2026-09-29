#ifndef RAT_CC_CHARCLASS_H
#define RAT_CC_CHARCLASS_H

#include "core.h"

namespace rat::cc {
	inline B32 isIdentStart(C8 c) {
		return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}
	inline B32 isIdentCont(C8 c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
	inline B32 isDigit(C8 c) { return c >= '0' && c <= '9'; }
	inline B32 isOctalDigit(C8 c) { return c >= '0' && c <= '7'; }
	inline B32 isHexDigit(C8 c) {
		return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}
	inline I32 hexVal(C8 c) {
		if(c >= '0' && c <= '9')
			return c - '0';
		if(c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if(c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}
	inline B32 simpleEscape(C8 e, U8& out) {
		constexpr C8 kFrom[] = {'n', 't', 'r', '\\', '\'', '"', 'a', 'b', 'f', 'v', 'e'};
		constexpr U8 kTo[] = {'\n', '\t', '\r', '\\', '\'', '"', '\a', '\b', '\f', '\v', 27};
		for(U32 i = 0; i < sizeof kTo; ++i) {
			if(kFrom[i] == e) {
				out = kTo[i];
				return true;
			}
		}
		return false;
	}
} // namespace rat::cc

#endif
