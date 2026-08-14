#ifndef RAT_CC_PREDEF_H
#define RAT_CC_PREDEF_H

#include "core.h"

#include "target/target.h"

namespace rat::cc {
	namespace detail {
		struct Def {
			const char* name;
			const char* value;
		};

		void append(String& out, const Def* defs, U32 n);
		void appendCommon(String& out);
		void appendLinux(String& out);
		void appendWindows(String& out);
		String generate(const TargetTriple& t);
	} // namespace detail

	const String& builtinPredefs(const TargetTriple& triple);
} // namespace rat::cc

#endif
