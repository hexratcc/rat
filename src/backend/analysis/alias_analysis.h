// alias analysis: decide whether two memory accesses may, must, or never
// refer to overlapping storage, used to drive MemoryOpt
//
// references:
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#ifndef RAT_ANALYSIS_ALIASANALYSIS_H
#define RAT_ANALYSIS_ALIASANALYSIS_H

#include "core.h"
#include "hash.h"
#include "ir/function.h"

namespace rat {
	struct Node;

	namespace detail {
		B32 idLess(const Node* a, const Node* b);
	} // namespace detail

	enum class AliasResult {
		NoAlias,
		MayAlias,
		MustAlias
	};

	struct AliasAnalysis {
		static constexpr U32 kMaxCarryCone = 32; // cone nodes mayCarryRestrict walks per address term

		AliasAnalysis(U32 pointerBytes, U64 restrictParams);

		AliasResult alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const;

		U32 getAccessSize(const Node* access) const;

		struct MustAliasKey {
			Node* base = nullptr; // underlying object (null => not comparable)
			I64 constant = 0;			// byte offset from base
			U32 size = 0;					// access size in bytes (0 => unknown)
			List<Node*> symbolic; // non-constant addends, sorted by id

			B32 valid() const { return base != nullptr && size != 0; }
			B32 operator==(const MustAliasKey& o) const {
				return base == o.base && constant == o.constant && size == o.size && symbolic == o.symbolic;
			}
		};

		struct MustAliasKeyHash {
			U64 operator()(const MustAliasKey& k) const {
				U64 h = kFnvBasis;
				hashMix(h, reinterpret_cast<U64>(k.base));
				hashMix(h, (U64)k.constant);
				hashMix(h, (U64)k.size);
				for(Node* s : k.symbolic)
					hashMix(h, reinterpret_cast<U64>(s));
				return h;
			}
		};

		MustAliasKey mustAliasKey(Node* access) const;

		// object identity, independent of any offset
		static B32 isIdentified(const Node* n); // an Alloc or a Global
		static B32 distinctObjects(const Node* a, const Node* b);
		static I32 paramIndex(const Node* n); // parameter of a Start projection, else -1
		static B32 isRestrictParam(I32 index, U64 restricts);
		static B32 mayCarryRestrict(List<const Node*> work, U64 restricts, U32 cap);
		static U64 restrictFacts(const Function& fn);
		Node* objectOf(Node* addr) const;
	private:
		struct Address {
			Node* base;						// pointer
			I64 constant;					// base offset
			List<Node*> symbolic; // non-constant addends
		};

		const Address& decompose(Node* addr) const;
		static Node* accessAddress(Node* n); // pointer operand of a Load/Store (else null)
	private:
		U32 ptrBytes;
		U64 restrictParams;
		mutable Map<Node*, Address> decomposeCache;
		mutable Map<String, Node*> symbolObjects; // first global node seen per symbol
		// first projection seen per restrict parameter
		mutable Node* paramObjects[kMaxRestrictParams] = {};
	};
} // namespace rat

#endif
