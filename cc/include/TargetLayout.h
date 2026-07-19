#ifndef RAT_CC_TARGETLAYOUT_H
#define RAT_CC_TARGETLAYOUT_H

#include "Core.h"

#include "Target/Target.h"
#include "Target/X86Asm.h"

namespace rat::cc {
	struct TargetLayout {
		U32 ptrBytes = 8;
		U32 longBits = 64;
		U32 wcharBytes = 4;
		B32 win64VaList = false;
		B32 isWindows = false;

		static TargetLayout forTriple(const TargetTriple& t) {
			TargetLayout l;
			l.isWindows = t.isWindows();
			l.longBits = l.isWindows ? 32 : 64;
			l.wcharBytes = l.isWindows ? 2 : 4;
			l.win64VaList = x86CallConv(t.os).vaList == X86VaList::CharPtr;
			return l;
		}
	};
} // namespace rat::cc

#endif
