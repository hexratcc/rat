// alias analysis: decide whether two memory accesses may, must, or never
// refer to overlapping storage, used to drive MemoryOpt
//
// references:
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#ifndef RAT_PASS_OPT_ALIASANALYSIS_H
#define RAT_PASS_OPT_ALIASANALYSIS_H

#include "Core.h"

namespace rat {
	struct Function;
	struct Node;

	enum class AliasResult { NoAlias, MayAlias, MustAlias };

	struct AliasAnalysis {
		explicit AliasAnalysis(const Function& fn);

		AliasResult alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const;
		AliasResult alias(Node* accessA, Node* accessB) const;

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
				U64 h = 1469598103934665603ull; // FNV-1a
				auto mix = [&](U64 v) {
					h ^= v;
					h *= 1099511628211ull;
				};
				mix((U64) reinterpret_cast<std::uintptr_t>(k.base));
				mix((U64)k.constant);
				mix((U64)k.size);
				for(Node* s : k.symbolic)
					mix((U64) reinterpret_cast<std::uintptr_t>(s));
				return h;
			}
		};

		MustAliasKey mustAliasKey(Node* access) const;
	private:
		struct Address {
			Node* base;						// pointer
			I64 constant;					// base offset
			List<Node*> symbolic; // non-constant addends
		};

		const Address& decompose(Node* addr) const;

		static B32 isIdentified(const Node* n);
		static B32 distinctObjects(const Node* a, const Node* b);
	private:
		const Function& fn;
		mutable Map<Node*, Address> decomposeCache;
	};
} // namespace rat

#endif
