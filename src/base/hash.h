#ifndef RAT_BASE_HASH_H
#define RAT_BASE_HASH_H

#include "core.h"

namespace rat {
	// FNV-1a 64-bit
	constexpr U64 kFnvBasis = 14695981039346656037ull;
	constexpr U64 kFnvPrime = 1099511628211ull;

	inline void hashMix(U64& h, U64 v) {
		h ^= v;
		h *= kFnvPrime;
	}
} // namespace rat

#endif
