#include "ir/debug_info.h"

namespace rat {
	// consecutive nodes mostly share a location, reuse the last entry then
	U32 DebugBody::addLoc(SrcLoc src) {
		const DebugLoc& last = locs.back();
		if(locs.size() > 1 && last.src.file == src.file && last.src.line == src.line &&
			 last.src.col == src.col)
			return (U32)locs.size() - 1;
		locs.push_back({src});
		return (U32)locs.size() - 1;
	}
} // namespace rat
