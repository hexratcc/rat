#ifndef RAT_CC_TARGETLAYOUT_H
#define RAT_CC_TARGETLAYOUT_H

#include "core.h"

#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat::cc {
	struct TargetLayout {
		U32 ptrBytes = 8;
		U32 longBits = 64;
		U32 wcharBytes = 4;
		B32 win64VaList = false;

		static TargetLayout forTriple(const TargetTriple& t) {
			TargetLayout l;
			B32 win = t.isWindows();
			l.longBits = win ? 32 : 64;
			l.wcharBytes = win ? 2 : 4;
			l.win64VaList = x86CallConv(t.os).vaList == X86VaList::CharPtr;
			return l;
		}
	};
} // namespace rat::cc

#endif
