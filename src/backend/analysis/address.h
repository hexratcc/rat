// address decomposition: a pointer as base + sum(scale * var) + constant, and
// the disjointness test that follows from two such forms

#ifndef RAT_ANALYSIS_ADDRESS_H
#define RAT_ANALYSIS_ADDRESS_H

#include "core.h"

namespace rat {
	struct AliasAnalysis;
	struct Node;

	// a pointer decomposed into base + sum(scale*var) + constant, over `size` bytes
	struct RefinedAddr {
		Node* base = nullptr;
		I64 constant = 0;
		List<Pair<const Node*, I64>> terms; // (var, scale), sorted by id
		U32 size = 0;												// access bytes

		B32 valid() const;
		B32 sameGroup(const RefinedAddr& o) const;
	};

	namespace detail {
		B32 constValue(const Node* n, I64& v);
		B32 isI64(const Node* n);
		B32 termLess(const Pair<const Node*, I64>& a, const Pair<const Node*, I64>& b);
		void refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		void refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
	} // namespace detail

	RefinedAddr refineAddr(Node* addr, U32 accessBytes);
	void canonicalizeTerms(List<Pair<const Node*, I64>>& terms);

	// decided from the refined addresses alone: either the objects differ, or the byte ranges do
	B32 provablyDisjoint(const RefinedAddr& ka, U32 sza, const RefinedAddr& kb, U32 szb);
	B32 provablyDisjoint(const AliasAnalysis& aa,
											 Node* pa,
											 const RefinedAddr& ka,
											 U32 sza,
											 Node* pb,
											 const RefinedAddr& kb,
											 U32 szb);
} // namespace rat

#endif
