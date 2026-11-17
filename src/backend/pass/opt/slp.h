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
		static constexpr U32 kMaxLanes = 4;
		// stores of a stretch, compared pairwise
		static constexpr U32 kMaxStretch = 64;
		static constexpr U32 kMaxOps = 64;	 // op tuples tried per seed tuple
		static constexpr U32 kMaxHops = 16;	 // stores walked above a wide load
		static constexpr U32 kMaxTerms = 64; // terms of one reduction
		static constexpr I32 kMinProfit = 2; // scalar ops a pack must save

		using Lanes = SmallList<Node*, kMaxLanes>;

		const C8* name() const override;
		void printStats(const Module& module, std::ostream& os) const override;
		U32 runOnFunction(Function& function, const TargetInfo& target) override;
	private:
		struct StoreAccess {
			StoreNode* store = nullptr;
			Addr addr;
			U32 size = 0;
		};

		// a vector node and the scalar lanes it replaces
		struct Vector {
			Lanes lanes;
			Node* node = nullptr;
			I32 cost = 0;
		};

		// the type bit of a scalar type, 0 for others
		static U8 typeBits(const Type* t);

		// stores
		B32 packChain(const List<StoreNode*>& chain);
		void packStretch(List<StoreAccess>& stretch);
		static B32 byAddress(const StoreAccess& a, const StoreAccess& b);
		U32 packWindow(const StoreAccess* window, U32 count);

		// reductions
		void packReduction(Node* root);
		static List<Node*> flatten(Node* root, Set<const Node*>& steps);
		Node* foldLanes(Opcode op, const List<Node*>& termVectors, Type* scalarType);

		// vectors
		List<Node*> tryPack(const List<Lanes>& tuples, Set<const Node*> dead, I32 gain);
		static B32 markDead(Node* n, Set<const Node*>& dead);
		Node* vectorize(const Lanes& lanes);
		Node* wideLoad(const Lanes& lanes);
		B32 loadSlots(const Lanes& lanes, LoadNode* lane0, U16& sel, Lanes& ordered);
		U32 elementOf(Node* n, LoadNode* lane0, U32 count);
		B32 forwardStalls(Node* state, Node* pointer, U32 bytes);
		B32 callWrites(Node* state, U32 object);
		Node* vectorOp(const Lanes& lanes);
		Node* blend(const Lanes& lanes, Node* node, Node* other);
		static B32 hasRule(const Node* lane);
		Type* laneType(const Node* lane);
		Node* gather(const Lanes& lanes);
		Node* addVector(const Lanes& lanes, Node* node, I32 cost);
		void rollback(U32 keep);
	private:
		Function* fn = nullptr;
		U32 ptrBytes = 8;
		AddrCache addrs;
		Set<U32> storedObjects;
		List<Vector> vectors;
		Map<const Node*, List<U32>> vectorsOf;
		U32 packStart = 0; // first vector of the pack under work
		U32 budget = 0;		 // op tuples left to try
		U32 packedWindows = 0;
		U32 packedReductions = 0;
	};
} // namespace rat

#endif
