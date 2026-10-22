// a pointer as base + sum(scale * var) + constant. Pointers with equal base and terms share a
// group id, and two accesses in one group are disjoint when their byte ranges do not overlap

#ifndef RAT_ANALYSIS_ADDRESS_H
#define RAT_ANALYSIS_ADDRESS_H

#include "core.h"

#include <map>

namespace rat {
	struct Node;

	enum class BaseKind : U8 {
		Unknown,	// may be based on any pointer
		Param,		// a pointer parameter
		Restrict, // a restrict parameter
		Object,		// allocation, stack slot or global symbol
	};

	struct Addr {
		I64 offset = 0;
		U32 group = 0;	// same for pointers with equal base and terms
		U32 object = 0; // object or restrict parameter of the base, 0 for none
		BaseKind kind = BaseKind::Unknown;
	};

	namespace detail {
		using Terms = List<Pair<const Node*, I64>>; // (var, scale) sorted, then (base, 0)

		constexpr U32 kMaxRefineDepth = 12; // nested integer ops refined into terms
		constexpr U32 kMaxPtrHops = 32;

		// object id = kObjectTags * number + tag
		constexpr U32 kObjectTags = 4;
		constexpr U32 kAllocTag = 1;		// node id of an Alloc or StackAlloc
		constexpr U32 kRestrictTag = 2; // parameter index, one object for all its projections
		constexpr U32 kGlobalTag = 3;		// global symbol, one object for all its Global nodes

		void refineTerm(const Node* n, I64 scale, B32 narrow, I64& constant, Terms& terms, U32 depth);
		void mergeTerms(Terms& terms);
	} // namespace detail

	struct AddrCache {
		AddrCache(U64 restrictParams, U32 ids); // ids: the node id bound
		Addr get(Node* ptr);
	private:
		Addr classify(const detail::Terms& terms);
	private:
		U64 restrictParams;
		List<U32> slots; // node id -> 1 + its index in addrs, 0 until computed
		List<Addr> addrs;
		std::map<detail::Terms, Addr> groups; // at offset 0
		Map<U32, Addr> bases;									// base id -> group of a pointer without terms
		detail::Terms key;										// scratch of get
		Map<String, U32> symbols;							// global symbol -> number
	};

	B32 disjointBases(const Addr& a, const Addr& b);
	B32 disjoint(const Addr& a, U32 sza, const Addr& b, U32 szb);
} // namespace rat

#endif
