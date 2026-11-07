// target-agnostic source-level debug description
#ifndef RAT_IR_DEBUGINFO_H
#define RAT_IR_DEBUGINFO_H

#include "core.h"

namespace rat {
	struct SrcLoc {
		U32 file = 0; // DebugInfo::files
		U32 line = 0;
		U32 col = 0;
	};

	// a source function, outlives its IR function so inlined copies can still name it
	struct DebugFunc {
		String name;
		SrcLoc decl;
		B32 external = false;
		B32 variadic = false;
	};

	struct DebugLoc {
		SrcLoc src;
	};

	// debug state of one IR function
	struct DebugBody {
		DebugFunc* func;
		List<DebugLoc> locs = List<DebugLoc>(1); // 0 = no location

		U32 addLoc(SrcLoc src);
	};

	struct DebugInfo {
		String producer;
		String compDir;
		List<String> files; // files[0] is the primary source
		List<UniquePtr<DebugFunc>> funcs;
	};
} // namespace rat

#endif
