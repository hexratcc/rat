// a pointer as base + sum(scale * var) + constant. Pointers with equal base and terms share a
// group id, and two accesses in one group are disjoint when their byte ranges do not overlap

#ifndef RAT_ANALYSIS_ADDRESS_H
#define RAT_ANALYSIS_ADDRESS_H

#include "core.h"

#include <map>

namespace rat {
	struct Node;

	struct Addr {
		I64 offset = 0;
		U32 group = 0;		 // same for pointers with equal base and terms
		U32 object = 0;		 // object or restrict parameter of the base, 0 for none
		B32 param = false; // the base is a pointer parameter, restrict or not
	};

	namespace detail {
		using Terms = List<Pair<const Node*, I64>>; // (var, scale) sorted, then (base, 0)

		constexpr U32 kMaxRefineDepth = 12; // nested integer ops refined into terms
		constexpr U32 kMaxPtrHops = 32;			// add and sub steps walked from a pointer to its base
		// a pointer further from its base gets a group of its own, so sums and differences of offsets fit I64
		constexpr I64 kMaxPtrOffset = (I64)1 << 61;

		// object id = kObjectTags * number + tag
		constexpr U32 kObjectTags = 4;
		constexpr U32 kAllocTag = 1;		// node id of an Alloc or StackAlloc
		constexpr U32 kRestrictTag = 2; // parameter index, one object for all its projections
		constexpr U32 kGlobalTag = 3;		// global symbol, one object for all its Global nodes

		// sum and product modulo 2^64, as pointer arithmetic wraps
		I64 wrapAdd(I64 a, I64 b);
		I64 wrapMul(I64 a, I64 b);
		void refineTerm(const Node* n, I64 scale, B32 narrow, I64& constant, Terms& terms, U32 depth);
		void mergeTerms(Terms& terms);
	} // namespace detail

	struct AddrCache {
		AddrCache(U64 restrictParams = 0, U32 idBound = 0);
		Addr get(Node* ptr);
	private:
		Addr decompose(Node* ptr); // ptr as its group plus a constant offset
		Addr classifyBase(const detail::Terms& terms);
	private:
		U64 restrictParams;
		List<U32> slotOf;													// node id -> 1 + its index in cached, 0 for none
		List<Addr> cached;												// the Addr of each pointer get has seen
		std::map<detail::Terms, Addr> termGroups; // terms key -> group, at offset 0
		Map<U32, Addr> baseGroups;								// base id -> group of a pointer without terms
		detail::Terms key;												// the termGroups key decompose builds
		Map<String, U32> symbols;									// global symbol -> number
	};

	B32 disjointBases(const Addr& a, const Addr& b);
	B32 disjoint(const Addr& a, U32 sizeA, const Addr& b, U32 sizeB);
} // namespace rat

#endif
