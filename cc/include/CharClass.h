#ifndef RAT_CC_CHARCLASS_H
#define RAT_CC_CHARCLASS_H

#include "Core.h"

namespace rat::cc {
	inline B32 isIdentStart(char c) {
		return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}
	inline B32 isIdentCont(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
	inline B32 isDigit(char c) { return c >= '0' && c <= '9'; }
	inline B32 isOctalDigit(char c) { return c >= '0' && c <= '7'; }
	inline B32 isHexDigit(char c) {
		return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}
	inline I32 hexVal(char c) {
		if(c >= '0' && c <= '9')
			return c - '0';
		if(c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if(c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}
	inline B32 simpleEscape(char e, U8& out) {
		switch(e) {
		case 'n':
			out = '\n';
			return true;
		case 't':
			out = '\t';
			return true;
		case 'r':
			out = '\r';
			return true;
		case '\\':
			out = '\\';
			return true;
		case '\'':
			out = '\'';
			return true;
		case '"':
			out = '"';
			return true;
		case 'a':
			out = '\a';
			return true;
		case 'b':
			out = '\b';
			return true;
		case 'f':
			out = '\f';
			return true;
		case 'v':
			out = '\v';
			return true;
		default:
			return false;
		}
	}
} // namespace rat::cc

#endif
