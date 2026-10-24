// load canonicalization: a load of an inner state of its block's store chain reads the earliest
// chain state that holds its bytes, but none above a state read in its address cone, so the
// schedule stays acyclic (C). slp moves it back when the chain packs nothing

#ifndef RAT_PASS_OPT_LOADCANON_H
#define RAT_PASS_OPT_LOADCANON_H

#include "core.h"

#include "pass/opt/slp/slp_pack_detail.h"
#include "pass/pass.h"

#include <array>

namespace rat {
	namespace slp {
		constexpr U32 kMaxCone = 64; // address cone nodes visited per load

		// a load loadcanon moved off an inner state of a chain
		struct Hoist {
			U32 chain = 0; // index in Handoff::chains
			LoadNode* load = nullptr;
			StoreNode* state = nullptr; // the state it read before
		};

		// what loadcanon leaves for slp on the same function: its hoists, and its chains and addresses
		// to save compile time
		struct Handoff {
			Handoff() = default;
			explicit Handoff(Function& fn); // chains and an empty cache, nothing hoisted

			const Function* fn = nullptr;
			U64 version = 0; // of fn when loadcanon finished
			List<Chain> chains;
			AddrCache addrs;
			List<Hoist> hoists;
		};

		// stores are numbered from 1 in chain order, 0 is none
		struct LastOutside {
			void add(U32 store, U32 group);
			U32 outside(U32 group) const; // the last store outside group

			U32 last = 0;
			U32 lastGroup = 0;
			U32 lastOther = 0; // the last store outside lastGroup
		};

		// the stores of a chain so far, by the bytes they may overlap
		struct Conflicts {
			void add(U32 store, const Access& a);
			U32 lastConflict(const Access& a) const; // the last store that may overlap a

			LastOutside all;														 // every store
			U32 lastWild = 0;														 // the last store without an object
			Map<U32, LastOutside> objects;							 // object -> its stores
			Map<U64, std::array<U32, kVecBytes>> blocks; // (group, 16-byte block) -> last store per byte
		};

		List<Chain> collectChains(Function& fn);
		StoreNode* chainSuccessor(const StoreNode* prev);
		void hoistLoads(Handoff& h, U32 ptrBytes, U32 chainIndex, List<U32>& stateOf);
		U32 coneState(LoadNode* l, const List<U32>& stateOf);
		Handoff& handoff();
		Handoff takeHandoff(Function& fn);
		void unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed);
	} // namespace slp

	struct LoadCanonPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
