#ifndef RAT_CC_PREDEF_H
#define RAT_CC_PREDEF_H

#include "core.h"

#include "target/target.h"

namespace rat::cc {
	namespace detail {
		struct Def {
			const C8* name;
			const C8* value;
		};

		struct TargetDef {
			const C8* name;
			const C8* linuxValue;
			const C8* windowsValue;
		};

		void appendDefine(String& out, const C8* name, const C8* value);
		void appendCommon(String& out);
		void appendTarget(String& out, B32 windows);
		String generate(const TargetTriple& t);
	} // namespace detail

	const String& builtinPredefs(const TargetTriple& triple);
} // namespace rat::cc

#endif
