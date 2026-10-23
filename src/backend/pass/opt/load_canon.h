// load canonicalization: a load of a block that reads an inner state of the block's store chain
// reads the earliest chain state that holds its bytes, but no state above one that a memory reader
// in its address cone reads, so the schedule stays acyclic. slp moves it back when it packs
// nothing in the chain

#ifndef RAT_PASS_OPT_LOADCANON_H
#define RAT_PASS_OPT_LOADCANON_H

#include "core.h"

#include "pass/opt/slp/slp_pack.h"
#include "pass/pass.h"

#include <array>

namespace rat {
	namespace slp {
		constexpr U32 kMaxCone = 64; // address cone nodes visited per load

		// stores are numbered from 1 in chain order, 0 is none
		struct LastOutside {
			void add(U32 store, U32 group);
			U32 outside(U32 group) const; // the last store outside group

			U32 last = 0;
			U32 lastGroup = 0; // the group of last
			U32 lastOther = 0; // the last store outside lastGroup
		};

		struct Conflicts {
			void add(U32 store, const Access& a);
			U32 lastConflict(const Access& a) const; // the last store that may overlap a

			LastOutside all;														 // every store
			U32 lastWild = 0;														 // the last store without an object
			Map<U32, LastOutside> objects;							 // object -> its stores
			Map<U64, std::array<U32, kVecBytes>> blocks; // (group, 16-byte block) -> last store per byte
		};

		B32 hasInnerReader(const Chain& chain);
		U32 coneState(LoadNode* l, const List<U32>& stateOf);
		void hoistLoads(Handoff& h, U32 ptrBytes, U32 chainIndex, List<U32>& stateOf);
	} // namespace slp

	struct LoadCanonPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
