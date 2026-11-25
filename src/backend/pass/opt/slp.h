// superword-level parallelism
//
// references:
// - S. Larsen and S. Amarasinghe, "Exploiting Superword Level Parallelism with
//   Multimedia Instruction Sets", PLDI, 2000
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995

#ifndef RAT_PASS_OPT_SLP_H
#define RAT_PASS_OPT_SLP_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"
#include "pass/pass.h"

namespace rat {
	struct LoadNode;
	struct StoreNode;
	struct Type;

	struct SlpPass : FunctionPass {
		static constexpr U32 kVecBytes = 16; // SSE
		static constexpr U32 kMaxLanes = 16;
		static constexpr U32 kMaxStretch = 64;		 // stores per stretch, compared pairwise
		static constexpr U32 kMaxOps = 64;				 // op tuples tried per seed
		static constexpr U32 kMaxUsers = 64;			 // users markDead looks at
		static constexpr U32 kMaxHops = 16;				 // states forwardStalls and loadsBeforeCall walk
		static constexpr U32 kMaxTerms = 64;			 // terms per reduction
		static constexpr I32 kMinProfit = 2;			 // scalar ops a pack must save
		static constexpr I32 kMinReduceProfit = 1; // of a reduction with no call after its loads

		using Lanes = SmallList<Node*, kMaxLanes>;
		using Terms = SmallList<Node*, 16>;

		// A store of a stretch with its address and size
		struct StoreAccess {
			StoreNode* store = nullptr;
			Addr addr;
			U32 size = 0;
		};

		// A vector node and the scalar lanes it replaces
		struct Vector {
			Lanes lanes;
			Node* node = nullptr;
			I32 cost = 0; // 0 when borrowed from an earlier vector, or free like a zero pack
		};
	public:
		void printStats(const Module& module, std::ostream& os) const override;
		U32 runOnFunction(Function& function, const TargetInfo& target) override;
	private:
		// stores
		B32 packChain(const List<StoreNode*>& chain);
		void packStretch(List<StoreAccess>& stretch);
		static B32 byAddress(const StoreAccess& a, const StoreAccess& b);
		B32 packWindow(const StoreAccess* window, U32 count);

		// reductions
		void packReduction(Node* root);
		static Terms flatten(Node* root, Terms& steps);
		Node* foldLanes(Node* root, const List<Node*>& termVectors, const Terms& terms);
		Node* combine(Node* root, Node* a, Node* b);
		static B32 isMinMax(const Node* node);
		static B32 sameStep(const Node* node, const Node* root);
		B32 loadsBeforeCall();

		// vectors
		List<Node*> tryPack(const List<Lanes>& tuples, Set<const Node*> dead, I32& gain, I32 minProfit);
		static B32 markDead(Node* lane, Set<const Node*>& dead);
		Node* vectorize(const Lanes& lanes);
		Node* wideLoad(const Lanes& lanes);
		Node* windowLoad(const Lanes& window, Type* type);
		B32 loadWindows(const Lanes& lanes, LoadNode* lane0, U16& sel, Lanes& low, Lanes& high);
		U32 slotOf(Node* node, LoadNode* lane0, U32 frameSize);
		B32 forwardStalls(LoadNode* first, U32 bytes);
		B32 callWrites(Node* state, U32 object);
		Node* vectorOp(const Lanes& lanes);
		B32 opFits(const Lanes& lanes, Opcode alt);
		Node* demote(const Lanes& lanes);
		static B32 demotable(const Node* lane, const Node* wide0);
		Node* narrowOperand(Node* wide, Type* narrowType);
		Node* matchedInput(Node* lane, Node* lane0, U32 k);
		B32 alike(Node* a, Node* b);
		Node* negOrNot(Opcode op, Type* type, Node* operand);
		Node* blend(const Lanes& lanes, Node* node, Node* other);
		Node* addSubShuffled(const Lanes& lanes);
		static B32 addSubOrder(const Lanes& lanes);
		static U8 typeBits(const Type* type);
		static B32 hasRule(const Node* lane);
		Type* laneType(const Node* lane);
		Node* gather(const Lanes& lanes);
		Node* addVector(const Lanes& lanes, Node* node, I32 cost);
		void rollback(U32 keep);
	private:
		Function* fn = nullptr;
		U32 ptrBytes = 8;
		AddrCache addrs;				// from loadcanon
		Set<U32> storedObjects; // objects the chains store to, see callWrites
		List<Vector> vectors;		// of the current stretch or reduction
		U32 packStart = 0;			// first vector of the current pack
		U32 budget = 0;					// op tuples left to try
		U32 packedWindows = 0;	// stats, see printStats
		U32 packedReductions = 0;
	};
} // namespace rat

#endif
