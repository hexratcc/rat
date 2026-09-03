// memory optimization: redundant-load elimination and store-to-load
// forwarding over the explicit memory-state edges of the SON,
// disambiguated by AliasAnalysis
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995 (memory as a value)
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#ifndef RAT_PASS_OPT_MEMORYOPT_H
#define RAT_PASS_OPT_MEMORYOPT_H

#include "core.h"
#include "analysis/alias_analysis.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct LoadNode;
	struct Node;
	struct PhiNode;
	struct StoreNode;
	struct Type;

	struct MemoryOptPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		U32 forwardStores(const AliasAnalysis& aa);
		U32 cseLoads(Function& fn, const AliasAnalysis& aa);
		U32 forwardLoopCarried(Function& fn, const AliasAnalysis& aa);

		// store-chain walk cap: unbounded it is O(loads * chain) and blows up on
		// huge by-value struct copies; hitting the cap only forgoes forwarding,
		// still sound
		static constexpr U32 kMaxStoreWalk = 512;

		// skip back over stores that provably do not alias [addr, addr+size)
		static Node* effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr, U32 size);

		struct ChainScan {
			StoreNode* store; // last must-alias store of matching size and type
			B32 clobbered;		// an aliasing store that cannot be forwarded intervenes
			B32 loops;				// the chain returns to the phi
		};
		static ChainScan scanPhiSide(const AliasAnalysis& aa, PhiNode* m, U32 side, LoadNode* l);
	private:
		struct BucketKey {
			Node* def;
			Type* type;
			AliasAnalysis::MustAliasKey addr;
			B32 operator==(const BucketKey& o) const {
				return def == o.def && type == o.type && addr == o.addr;
			}
		};

		struct BucketKeyHash {
			U64 operator()(const BucketKey& k) const {
				U64 h = AliasAnalysis::MustAliasKeyHash{}(k.addr);
				h ^= reinterpret_cast<U64>(k.def) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				h ^= reinterpret_cast<U64>(k.type) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				return h;
			}
		};

		List<LoadNode*> loads;
		List<Node*> defs;
		std::unordered_map<BucketKey, List<LoadNode*>, BucketKeyHash> buckets;
	};
} // namespace rat

#endif
