// SLP vectorization. Windows of stores in store chains and store sets, and integer reductions,
// pack into SSE vectors

#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"
#include "pass/pass.h"

#include <tuple>

namespace rat {
	struct Node;
	struct CallNode;
	struct StoreNode;
	struct LoadNode;
	struct MemMergeNode;
	struct Type;

	struct SlpStats {
		U32 windowsSeen = 0;		 // windows and reduction roots examined
		U32 packedUnguarded = 0; // windows fused statically
		U32 packedGuarded = 0;	 // windows fused behind a runtime guard
		U32 packedReduction = 0; // reductions packed into vectors
		U32 guardedRuns = 0;		 // merged guard regions emitted
		U32 guardChecks = 0;		 // runtime disjointness checks emitted
		U32 rejected = 0;				 // windows rejected early, and seeds that did not pack
	};

	namespace slp {
		constexpr U32 kVecShift = 4;							 // log2 kVecBytes
		constexpr U32 kVecBytes = 1u << kVecShift; // SSE baseline
		constexpr U32 kMaxLanes = 4;							 // i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;							 // most inputs of a lane op, a select has 3
		constexpr U32 kNoVector = ~0u;						 // no plan vector
		constexpr Opcode kNoOp = Opcode::Start;		 // no opcode
		constexpr I32 kMinProfit = 2;							 // scalar ops saved per window, net of lane plumbing
		constexpr U32 kMaxRunWindows = 4;					 // windows sharing one guard branch
		constexpr U32 kMaxGuards = 4;							 // runtime checks per guarded run
		constexpr U32 kMaxGuardedRuns = 16;				 // guarded runs per block, see Plan::build
		constexpr I32 kGuardCheckCost = 6;				 // ops per runtime disjointness check
		constexpr I32 kGuardBranchCost = 6;				 // branch + else-arm bloat per guarded run
		constexpr U32 kMinTerms = 4;							 // fewest terms of a reduction
		constexpr U32 kMaxTerms = 64;							 // most terms of one reduction
		constexpr U32 kReadPiece = 4;							 // bytes of one entry of the reads index
		// offset bound of a known access, so an offset and its neighbors fit the low half of a pairKey
		constexpr I64 kMaxOffset = INT32_MAX - (I64)kVecBytes;
		// work bounds, so each search stays linear in the function
		constexpr U32 kMaxOps = 64;				// Op vectors tried per seed tuple
		constexpr U32 kMaxSetGroups = 16; // groups of a set, compared pairwise by their bases
		constexpr U32 kMaxHops = 16;			// stores walked between a lane load's state and memIn
		constexpr U32 kMaxStlfHops = 16;	// states walked back to the stores a wide load reads

		// lane type classes of the op table
		enum TypeBits : U8 { kI8 = 1, kI16 = 2, kI32 = 4, kI64 = 8, kF32 = 16, kF64 = 32, kPtr = 64 };
		constexpr U8 kI32I64 = kI32 | kI64, kFp = kF32 | kF64, kElem = kI32I64 | kFp | kPtr;

		using Lanes = SmallList<Node*, kMaxLanes>; // a tuple, one scalar per lane
		using Chain = List<StoreNode*>;						 // a block's stores in chain order, or a set's run

		// a load loadcanon moved off an inner state of a chain
		struct Hoist {
			U32 chain = 0; // index in Handoff::chains
			LoadNode* load = nullptr;
			StoreNode* state = nullptr; // the state it read before
		};

		// what loadcanon leaves for slp on the same function, to save compile time
		struct Handoff {
			const Function* fn = nullptr;
			U64 version = 0; // of fn when loadcanon finished
			List<Chain> chains;
			AddrCache addrs;
			List<Hoist> hoists;
		};
		Handoff& handoff();
		Handoff takeHandoff(Function& fn);
		Handoff freshHandoff(Function& fn); // chains and an empty cache, nothing hoisted

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		// laneCount stores of group from chain[start] fill the bytes from offset, lane k is laneStore[k]
		struct Window {
			U32 start = 0;
			U32 laneCount = 0;
			U32 group = 0;
			I64 offset = 0;
			StoreNode* laneStore[kMaxLanes] = {};
		};

		// the tuples to pack for a run of stores or a reduction
		struct Seed {
			List<Lanes> tuples; // one vector each
			// scalar ops saved besides the plan's vectors, less kMinProfit per window or reduction
			I32 profit = 0;
			// a run of stores
			SmallList<StoreNode*, kMaxRunWindows * kMaxLanes> stores; // in chain order
			SmallList<StoreNode*, kMaxRunWindows> lowestStores;				// the lane 0 store of each window
			MemMergeNode* set = nullptr;															// the run's set, null in a chain
			// a reduction
			Node* root = nullptr;
			List<Node*> steps;			 // the ops the vectors replace
			List<Node*> scalarTerms; // terms left as scalars
		};

		// Gather: a Pack or Splat, Load: wide loads or a broadcast load, Op: lane ops as vector ops
		enum class VectorKind : U8 { Gather, Load, Op };

		// one vector of a plan
		struct Vector {
			Lanes lanes;
			VectorKind kind = VectorKind::Gather;
			I32 profit = 0; // scalar ops saved, negative for a gather
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp; // opcode of the lanes sel takes from the second op
			U32 operands[kMaxInputs] = {kNoVector, kNoVector, kNoVector}; // vector of each Op input
			Node* shiftAmount = nullptr;
			Node* mem = nullptr;						// state the loads read
			LoadNode* lowestLoad = nullptr; // lane load at the lowest offset
			I64 offset = 0;									// of the first wide load, or of the one load
			I64 end = 0;										// end of the highest lane
			U32 wideLoads = 0;							// 1 or 2, 0 for one load and a broadcast
			B32 permuted = false;						// the lanes are permuted in the wide loads
			B32 needsGuard = false;					// a lane load crosses a weak edge
			U16 sel = 0;										// ShuffleNode selector of the permute or the blend
			Node* node = nullptr;						// what apply emitted
		};

		struct GuardRange {
			U32 group = 0;
			Node* ptr = nullptr; // pointer of the access at begin
			I64 begin = 0;
			I64 end = 0;
		};

		struct LaneOps {
			Opcode opcode[kMaxLanes] = {}; // Neg as Sub from 0, Not as Xor with -1
			Node* input[kMaxLanes][kMaxInputs] = {};
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp;
		};

		struct GuardSplit {
			Node* block;
			StoreNode* firstStore; // of the run, its memory follows every rewire of memIn
			Node* phi;						 // final state
			Node* firstIf;				 // first guard branch
			Node* region;
		};

		struct Plan;

		// chains, sets and seeds
		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		StoreNode* chainSuccessor(const StoreNode* prev);
		List<Chain> collectChains(Function& fn);
		B32 isDisjointSet(const List<Access>& accesses, U32 first, U32 last, Map<U64, U16>& written);
		void lowerSet(MemMergeNode* set);
		void unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed);
		void addWindow(Seed& seed, const Chain& chain, const Window& window);
		List<Node*> flattenReduction(Node* root, List<Node*>& steps);
		// type classes, keys and costs
		U8 typeBits(const Type* t);
		U64 pairKey(U32 high, I64 low);
		I32 gatherCost(const Lanes& lanes, B32 reduction);
		B32 isSplat(const Lanes& lanes);
		B32 isUniformShift(const Lanes& lanes);
		I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel);
		I32 profitMargin(const Plan& plan);
		Node* memIn(const Seed& seed);
		B32 isInner(const Seed& seed, const Node* state);

		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats);
			U32 run();
			B32 pack(const Seed& seed);
			Type* laneType(const Node* n);
			// sets
			U32 packChains(const List<Chain>& chains, List<B32>& packed);
			MemMergeNode* mergeStretch(const Chain& chain, U32 first, U32 last);
			// seed
			B32 windowAt(const Chain& chain, U32 start, U32 bytes, Window& window);
			U32 packChain(const Chain& chain, MemMergeNode* set);
			U32 packSet(MemMergeNode* set);
			U32 packRunAt(const Chain& chain, U32 start, U32 bytes, MemMergeNode* set);
			void extendRun(const Chain& chain, const Window& first, U32 bytes, Seed& seed);
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 packReduction(Node* root);
			// plan queries
			B32 bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl);
			void indexReads();
			B32 storesTo(U32 object);
			void indexStores();
			// check
			void setRoles(const Plan& plan, B32 on);
			U8 roleOf(const Node* n) const;
			U32 markKeptLanes(const Plan& plan);
			B32 usedOutside(const Node* n, B32 copies) const;
			B32 laneDies(const Node* n) const;
			B32 innerReadersDie(const Seed& seed) const;
			// apply
			void apply(Plan& plan);
			void emitVectors(Plan& plan);
			Node* emitGather(const Vector& vec, Type* vecTy);
			Node* emitLoad(const Vector& vec, Type* vecTy, Plan& plan);
			Node* loadAt(const Vector& vec, Type* ty, I64 offset, Plan& plan);
			Node* emitOp(const Vector& vec, Type* vecTy, const Plan& plan);
			Node* makeOp(Opcode op, Type* vecTy, Node* const* inputs);
			void seedAnchors(const Plan& plan);
			Node* anchorPtr(Node* ptr, I64 offset, const Node* ctrl);
			Pair<Node*, I64> anchorOf(Node* ptr);
			Node* emitWideStores(const Plan& plan, Node* ctrl);
			void commitRun(const Plan& plan);
			void commitGuarded(const Plan& plan);
			void moveToElse(const Plan& plan, Node* block, Node* elseP);
			Node* emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP);
			void splitBlocks();
			B32 moveLaterUsers(const GuardSplit& split, const Map<const Node*, U32>& runAt);
			void commitReduction(const Plan& plan);

			// function, target and stats
			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			SlpStats& stats;
			// address facts
			AddrCache addrs;
			Map<U64, Pair<Node*, I64>> anchors; // (control, group) -> (anchor ptr, its offset)
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of 4-byte pieces loads read
			B32 readsIndexed = false;
			Set<U32> storedObjects; // objects the stores of fn write
			B32 storesIndexed = false;
			// guarded runs
			List<GuardSplit> splits;					 // for splitBlocks
			Map<const Node*, U32> guardedRuns; // block -> its guarded runs
			// plan check
			List<U8> roles; // node id -> Role, else 0
		};

		// the vectors of one seed and the runtime checks they need
		struct Plan {
			Plan(Slp& slp, const Seed& seed);
			B32 build();
			void addGuard(const Vector& vec);
			U32 planVector(const Lanes& lanes);
			U32 planGather(const Lanes& lanes);
			// Load
			U32 planLoad(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Vector& vec);
			B32 permuteSpan(const Lanes& lanes, Vector& vec);
			B32 loadState(const Lanes& lanes, Vector& vec);
			B32 canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard);
			B32 forwardBlocked(const Vector& vec);
			B32 splitsLoad(Node* state, const Access& load, SmallList<Node*, kMaxStlfHops>& walk);
			B32 callWrites(const CallNode* call, const Addr& addr);
			// Op
			U32 planOp(const Lanes& lanes);
			B32 readLaneOps(const Lanes& lanes, LaneOps& ops);
			B32 hasRuleForOps(const LaneOps& ops, Node* lane0);
			B32 hasRule(Opcode op, Node* lane);
			U32 operandTuples(const Lanes& lanes, const LaneOps& ops, Lanes* tuples);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orientOperands(const LaneOps& ops, Lanes* tuples);
			// apply
			Node* tupleNode(U32 tuple) const;

			Slp& slp;
			const Seed& seed;
			List<Vector> vectors;												// operands first
			SmallList<U32, kMaxTerms / 4> tupleVectors; // vector of each seed tuple
			List<GuardRange> guards;										// one runtime check each
			U32 budget = 0; // Op vectors left to try, kMaxOps more per seed tuple
			// the loads apply emitted with their address, so loadAt reuses an equal load
			List<Pair<LoadNode*, Addr>> loads;
		};
	} // namespace slp

	struct SlpPackPass : FunctionPass {
		const C8* name() const override;
		void beginModule(Module& module, const CallGraph& graph) override;
		void endModule(Module& module) override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		SlpStats stats;
	};
} // namespace rat

#endif
