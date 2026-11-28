#ifndef RAT_BASE_RELOC_H
#define RAT_BASE_RELOC_H

#include "core.h"

namespace rat {
	enum class RelocKind : U32 {
		Abs64 = 1,	// absolute 64-bit address       (S + A)
		Pc32 = 2,		// 32-bit pc-relative            (S + A - P), used by lea(rip)
		Plt32 = 4,	// 32-bit pc-relative call       (L + A - P), call to a function
		Abs32 = 10, // 32-bit absolute              (S + A), debug section offsets
	};

	struct AsmReloc {
		U32 offset;
		String symbol;
		RelocKind kind;
		I64 addend;
	};
} // namespace rat

#endif
