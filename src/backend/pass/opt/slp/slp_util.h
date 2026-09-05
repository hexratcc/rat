#ifndef RAT_PASS_OPT_SLP_UTIL_H
#define RAT_PASS_OPT_SLP_UTIL_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"

namespace rat {
	struct Node;
	struct LoadNode;
	struct Type;

	namespace slp {
		constexpr U32 kVecBytes = 16; // SSE baseline

		// bounded-depth structural hashing
		struct ShapeHash {
			static constexpr U32 kDepth = 4;

			static U64 mix(U64 h, U64 v);
			U64 shape(const Node* n, U32 depth);
			U64 operator()(const Node* n);

			Map<const Node*, U64> memo; // at kDepth
		};

		B32 envFlag(const C8* name);

		B32 packableElem(const Type* t);
		B32 packableBinary(Opcode op, const Type* t);
		String groupSig(const RefinedAddr& k);

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
