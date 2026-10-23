// load canonicalization: a load of a block that reads an inner state of the block's store chain
// reads the earliest chain state that holds its bytes, but no state above one that a memory reader
// in its address cone reads, so the schedule stays acyclic

#ifndef RAT_PASS_OPT_LOADCANON_H
#define RAT_PASS_OPT_LOADCANON_H

#include "core.h"

#include "pass/opt/slp/slp_pack.h"
#include "pass/pass.h"

#include <array>

namespace rat {
	namespace slp {
		constexpr U32 kMaxCone = 64; // address cone nodes visited per hoisted load

		struct LastOutside {
			void add(U32 at, U32 group);
			U32 outside(U32 group) const; // the last store outside group, 0 for none

			U32 last = 0, lastGroup = 0, lastOther = 0;
		};

		struct Conflicts {
			void add(U32 at, const Access& a);
			U32 lastConflict(const Access& a) const;

			LastOutside all;														 // every store
			U32 lastWild = 0;														 // the last store without an object
			Map<U32, LastOutside> objects;							 // object -> its stores
			Map<U64, std::array<U32, kVecBytes>> blocks; // (group, 16-byte block) -> last store per byte
		};

		B32 readsInner(const List<Chain>& chains);
		U32 coneState(LoadNode* l, const List<U32>& index);
		U32 hoistLoads(AddrCache& addrs, U32 ptrBytes, const Chain& chain, List<U32>& index);
	} // namespace slp

	struct LoadCanonPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
