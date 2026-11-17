// load canonicalization: a load of an inner state of its block's store chain reads the earliest
// chain state that holds its bytes, but none above a state read in its address cone, so the
// schedule stays acyclic (C). superword moves it back when the chain packs nothing

#ifndef RAT_PASS_OPT_LOADCANON_H
#define RAT_PASS_OPT_LOADCANON_H

#include "core.h"

#include "analysis/address.h"
#include "pass/pass.h"

#include <array>

namespace rat {
	struct LoadNode;
	struct StoreNode;

	struct LoadCanonPass : FunctionPass {
		static constexpr U32 kMaxCone = 64; // address cone nodes visited per load
		static constexpr U32 kVecShift = 4;
		static constexpr U32 kVecBytes = 1u << kVecShift; // bytes of a Conflicts block
		// accessOf drops a larger offset, so an offset and its neighbors fit the low half of a pairKey
		static constexpr I64 kMaxOffset = INT32_MAX - (I64)kVecBytes;

		using Chain = List<StoreNode*>; // in chain order

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		// a load loadcanon moved off an inner state of a chain
		struct Hoist {
			U32 chain = 0; // index in Handoff::chains
			LoadNode* load = nullptr;
			StoreNode* state = nullptr; // the state it read before
		};

		// what loadcanon leaves for superword on the same function, its hoists, chains and addresses
		// to save compile time
		struct Handoff {
			Handoff() = default;
			explicit Handoff(Function& fn); // chains and an empty cache, nothing hoisted
		public:
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
		private:
			U32 last = 0;
			U32 lastGroup = 0;
			U32 lastOther = 0; // the last store outside lastGroup
		};

		// the stores of a chain so far, by the bytes they may overlap
		struct Conflicts {
			void add(U32 store, const Access& a);
			U32 lastConflict(const Access& a) const; // the last store that may overlap a
		private:
			LastOutside all;							 // every store
			U32 lastWild = 0;							 // the last store without an object
			Map<U32, LastOutside> objects; // object -> its stores
			// (group, 16-byte block) -> last store per byte
			Map<U64, std::array<U32, kVecBytes>> blocks;
		};
	public:
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
		static U64 pairKey(U32 high, I64 low);
		static I64 blockEnd(I64 from, I64 end);
		static Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		static B32 hasInnerReader(const Chain& chain);
		static List<Chain> collectChains(Function& fn);
		static StoreNode* chainSuccessor(const StoreNode* prev);
		static void hoistLoads(Handoff& h, U32 ptrBytes, U32 chainIndex, List<U32>& stateOf);
		static U32 coneState(LoadNode* l, const List<U32>& stateOf);
		static Handoff& handoff();
		static Handoff takeHandoff(Function& fn);
		static void unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed);
	};
} // namespace rat

#endif
