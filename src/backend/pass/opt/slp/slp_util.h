#ifndef RAT_PASS_OPT_SLP_UTIL_H
#define RAT_PASS_OPT_SLP_UTIL_H

#include "core.h"

#include "ir/opcode.h"

namespace rat {
	struct Node;
	struct StoreNode;
	struct LoadNode;
	struct BinaryNode;
	struct Type;
	struct AliasAnalysis;

	namespace slp {
		constexpr U32 kVecBytes = 16; // SSE baseline

		// a pointer decomposed into base + sum(scale*var) + constant, over `size` bytes
		struct RefinedAddr {
			Node* base = nullptr;
			I64 constant = 0;
			List<Pair<const Node*, I64>> terms; // (var, scale), sorted by id
			U32 size = 0;												// access bytes

			B32 valid() const;
			B32 sameGroup(const RefinedAddr& o) const;
		};

		// bounded-depth structural hashing
		struct ShapeHash {
			static constexpr U32 kDepth = 4;
			Map<const Node*, U64> memo; // at kDepth

			static U64 mix(U64 h, U64 v);
			U64 shape(const Node* n, U32 depth);
			U64 operator()(const Node* n);
		};

		B32 envFlag(const C8* name);

		B32 packableElem(const Type* t);
		B32 packableBinary(Opcode op, const Type* t);
		B32 identifiedBase(const Node* n);
		B32 isI64(const Node* n);
		B32 constValue(const Node* n, I64& v);

		void refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		void refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		RefinedAddr refineAddr(Node* addr, U32 accessBytes);
		void canonicalizeTerms(List<Pair<const Node*, I64>>& terms);
		String groupSig(const RefinedAddr& k);
		B32 provablyDisjoint(const RefinedAddr& ka, U32 sza, const RefinedAddr& kb, U32 szb);
		B32 provablyDisjoint(const AliasAnalysis& aa,
												 Node* pa,
												 const RefinedAddr& ka,
												 U32 sza,
												 Node* pb,
												 const RefinedAddr& kb,
												 U32 szb);

		U32 laneCountFor(U32 esz);
		B32 supportedEsz(U32 esz);

		B32 dataCone(const Node* root, U32 cap, List<const Node*>& out);
		LoadNode* firstLoadInCone(Node* root, U32 cap);
		B32 coneEndsInStores(const Node* seed, const Set<const Node*>& runStores, U32 cap);
		B32 usesValue(const Node* u, const Node* x);
		void rewriteInput(Node* u, const Node* from, Node* to);
	} // namespace slp
} // namespace rat

#endif
