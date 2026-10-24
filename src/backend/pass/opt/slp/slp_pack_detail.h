// SLP internals by stage
// 1. chains, slp_chain.cpp, Slp: windows of stores along a chain or a set make one Seed per run
// 2. plan, slp_plan.cpp and slp_load.cpp, Plan: the vectors and runtime checks of a Seed
// 3. check, slp_pack.cpp, Check: the profit covers the kept lanes, every inner reader dies (L3)
// 4. apply, slp_apply.cpp, Emitter: the vectors, then the wide stores, guarded if a lane needs it
// 5. reductions, slp_reduce.cpp, Slp: each reduction root is a Seed for stages 2 to 4

#ifndef RAT_PASS_OPT_SLPPACKDETAIL_H
#define RAT_PASS_OPT_SLPPACKDETAIL_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"
#include "pass/opt/slp/slp_pack.h"

#include <tuple>

namespace rat {
	struct Node;
	struct CallNode;
	struct StoreNode;
	struct LoadNode;
	struct MemMergeNode;
	struct PhiNode;
	struct Type;

	namespace slp {
		constexpr U32 kVecShift = 4;
		constexpr U32 kVecBytes = 1u << kVecShift; // SSE baseline
		constexpr U32 kMaxLanes = 4;							 // i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;							 // inputs of a lane op, a select has 3
		constexpr U32 kNoVector = ~0u;
		constexpr Opcode kNoOp = Opcode::Start;

		constexpr I32 kMinProfit = 2;				// scalar ops a window or a reduction must save
		constexpr I32 kGuardCheckCost = 6;	// ops per runtime disjointness check
		constexpr I32 kGuardBranchCost = 6; // branch and else-arm bloat per guarded run
		constexpr U32 kMaxRunWindows = 4;		// windows of one run, which share one guard
		constexpr U32 kMaxGuards = 4;				// runtime checks per guarded run
		constexpr U32 kMaxGuardedRuns = 16; // per block, as loads above a guard stay live across splits
		constexpr U32 kMinTerms = 4;				// fewest terms of a reduction
		constexpr U32 kMaxTerms = 64;				// most terms of a reduction
		constexpr U32 kReadPiece = 4;				// bytes per entry of the reads index
		// accessOf drops a larger offset, so an offset and its neighbors fit the low half of a pairKey
		constexpr I64 kMaxOffset = INT32_MAX - (I64)kVecBytes;
		// work bounds, so each search stays linear in the function
		constexpr U32 kMaxOps = 64;				// Op vectors tried per seed tuple
		constexpr U32 kMaxSetGroups = 16; // groups of a set, compared pairwise by their bases
		constexpr U32 kMaxHops = 16;			// states walked between a load and memIn, or to its writers

		// type classes as bits, so a mask names several
		enum TypeBits : U8 { kI8 = 1, kI16 = 2, kI32 = 4, kI64 = 8, kF32 = 16, kF64 = 32, kPtr = 64 };
		constexpr U8 kI32I64 = kI32 | kI64;
		constexpr U8 kFp = kF32 | kF64;
		constexpr U8 kElem = kI32I64 | kFp | kPtr;

		using Lanes = SmallList<Node*, kMaxLanes>; // a tuple, one scalar per lane, all of one type
		using Chain = List<StoreNode*>;						 // in chain order, or a set's consecutive stores

		U8 typeBits(const Type* t);
		U64 pairKey(U32 high, I64 low);
		I64 blockEnd(I64 from, I64 end);

		// what the stages share. Each lazy index is built at its first query and never updated, so that
		// moment shapes the output
		struct Context {
			Context(Function& fn, const TargetInfo& target, SlpGuards guards);
			Type* laneType(const Node* n); // a compare's is its mask, an int as wide as its operands
			U32 guardedRunsIn(const Node* block) const;
			B32 bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl); // slp_load.cpp
			B32 storesTo(U32 object);																					// slp_load.cpp

			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			SlpGuards guards;
			AddrCache addrs;													 // from the loadcanon handoff
			Map<const Node*, U32> guardedRunsPerBlock; // block -> guarded runs, commitGuarded counts
		private:
			void indexReads();
			void indexStores();
		private:
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of the 4-byte pieces loads read
			B32 readsIndexed = false;
			Set<U32> storedObjects;
			B32 storesIndexed = false;
		};

		// 1. chains

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		// laneCount stores of group from chain[start] fill bytes from offset, laneStore in offset order
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
			I32 profit = 0;			// scalar ops saved besides the plan's vectors
			// a run
			SmallList<StoreNode*, kMaxRunWindows * kMaxLanes> stores; // chain order, in a set by offset
			SmallList<StoreNode*, kMaxRunWindows> lowestStores;				// the lane 0 store of each window
			// a reduction
			Node* root = nullptr;
			List<Node*> steps; // the ops the vectors replace
			List<Node*> scalarTerms;
		};

		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		B32 hasInnerReader(StoreNode* const* stores, U32 count);
		void addWindow(Seed& seed, const Chain& chain, const Window& window);
		void lowerSet(MemMergeNode* set);
		Node* memIn(const Seed& seed);
		B32 isInner(const Seed& seed, const Node* state);
		MemMergeNode* setOf(const Seed& seed);

		// 2. plan

		// Gather: a Pack or Splat, Load: wide loads or a broadcast load, Op: lane ops as vector ops
		enum class VectorKind : U8 { Gather, Load, Op };

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
			B32 permuted = false;
			B32 needsGuard = false; // a lane load has a weak edge (L4)
			U16 sel = 0;						// ShuffleNode selector of the permute or the blend
			Node* node = nullptr;		// what apply emitted
		};

		// one runtime check, the loads of group read [begin, end), ptr points at begin
		struct GuardRange {
			U32 group = 0;
			Node* ptr = nullptr;
			I64 begin = 0;
			I64 end = 0;
		};

		// the lanes of an Op vector as one opcode each, with their operand tuples
		struct LaneOps {
			Opcode opcode[kMaxLanes] = {}; // Neg as Sub from 0, Not as Xor with -1
			Lanes operands[kMaxInputs];		 // tuple k holds input k of each lane
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp;
		};

		B32 isSplat(const Lanes& lanes);
		I32 gatherCost(const Lanes& lanes, B32 reduction);
		B32 pickOpcodes(LaneOps& ops, U32 laneCount);
		B32 isUniformShift(const Lanes& lanes);
		I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel);

		// the vectors of one seed and the runtime checks they need
		struct Plan {
			Plan(Context& cx, const Seed& seed);
			B32 build();
			Node* tupleNode(U32 tuple) const; // what apply emitted for seed tuple

			const Seed& seed;
			List<Vector> vectors;												// operands first
			SmallList<U32, kMaxTerms / 4> tupleVectors; // vector of each seed tuple
			List<GuardRange> guards;
			List<Pair<LoadNode*, Addr>> loads; // the loads apply made, loadAt reuses them
		private:
			void addGuard(const Vector& vec);
			U32 planVector(const Lanes& lanes);
			U32 planGather(const Lanes& lanes);
			// Op
			U32 planOp(const Lanes& lanes);
			B32 readLaneOps(const Lanes& lanes, LaneOps& ops);
			B32 hasRuleForOps(const LaneOps& ops, Node* lane0);
			B32 hasRule(Opcode op, Node* lane);
			U32 operandTuples(const Lanes& lanes, LaneOps& ops);
			void orientOperands(LaneOps& ops);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			// Load, slp_load.cpp
			U32 planLoad(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Vector& vec);
			B32 permuteSpan(const Lanes& lanes, Vector& vec);
			B32 loadState(const Lanes& lanes, Vector& vec);
			B32 canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard);
			B32 forwardBlocked(const Vector& vec);
			B32 splitsLoad(Node* state, const Access& load, SmallList<Node*, kMaxHops>& walk);
			B32 callWrites(const CallNode* call, const Addr& addr);
		private:
			Context& cx;
			U32 budget = 0; // Op vectors left to try, kMaxOps more per seed tuple
		};

		// 3. check

		I32 profitMargin(const Plan& plan, SlpGuards guards);

		// the role of each node in the plan under check and apply
		struct Check {
			explicit Check(const Function& fn);
			void setRoles(const Plan& plan, B32 on);
			B32 innerReadersDie(const Seed& seed) const;
			B32 laneDies(const Node* n) const;
			U32 markKeptLanes(const Plan& plan);
		private:
			B32 usedOutside(const Node* n, B32 copies) const;
			U8 roleOf(const Node* n) const;
		private:
			const Function& fn;
			List<U8> roles; // node id -> detail::Role, set while one plan is checked and applied
		};

		// 4. apply

		// a run behind runtime guards, its block splits after all chains
		struct GuardedRun {
			Node* block = nullptr;
			StoreNode* firstStore = nullptr; // its memory follows every rewire of memIn
			PhiNode* phi = nullptr;					 // final state, input 0 is the region that merges the arms
			Node* firstIf = nullptr;
		};

		// emits applied plans, at addresses off one anchor per (control, group)
		struct Emitter {
			explicit Emitter(Context& cx);
			void seedAnchors(const Plan& plan);
			void emitVectors(Plan& plan);
			void commitRun(const Plan& plan);
			void commitGuarded(const Plan& plan, const Check& check);
			void commitReduction(const Plan& plan); // slp_reduce.cpp
			void splitBlocks();
		private:
			Pair<Node*, I64> anchorOf(Node* ptr);
			Node* anchorPtr(Node* ptr, I64 offset, const Node* ctrl);
			Node* emitGather(const Vector& vec, Type* vecTy);
			Node* emitLoad(const Vector& vec, Type* vecTy, Plan& plan);
			Node* loadAt(const Vector& vec, Type* ty, I64 offset, Plan& plan);
			Node* emitOp(const Vector& vec, Type* vecTy, const Plan& plan);
			Node* makeOp(Opcode op, Type* vecTy, Node* const* inputs);
			Node* emitWideStores(const Plan& plan, Node* ctrl);
			Node* emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP);
			void moveToElse(const Plan& plan, Node* block, Node* elseP, const Check& check);
			B32 moveLaterUsers(const GuardedRun& guarded, const Map<const Node*, U32>& runAt);
		private:
			Context& cx;
			Map<U64, Pair<Node*, I64>> anchors; // (control, group) -> anchor and its offset
			List<GuardedRun> guardedRuns;				// commitGuarded adds, splitBlocks reads
		};

		// 5. reductions

		List<Node*> flattenReduction(Node* root, List<Node*>& steps);

		// the pass on one function, it searches the seeds and runs each through the stages
		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats, SlpGuards guards);
			Slp(const Slp&) = delete; // the stages hold references to cx
			Slp& operator=(const Slp&) = delete;
			U32 run();
		private:
			B32 pack(const Seed& seed);
			void apply(Plan& plan); // slp_apply.cpp
			// 1. chains
			U32 packChains(const List<Chain>& chains, List<B32>& packed);
			B32 isDisjointSet(const Chain& chain, U32 first, U32 last, Map<U64, U16>& written);
			U32 packWindows(const Chain& chain);
			U32 packRunAt(const Chain& chain, U32 start, U32 bytes);
			B32 windowAt(const Chain& chain, U32 start, U32 bytes, Window& window);
			U32 packSet(const Chain& chain, U32 first, U32 last);
			MemMergeNode* mergeStretch(const Chain& chain, U32 first, U32 last);
			// 5. reductions
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 packReduction(Node* root);
		private:
			Context cx;
			SlpStats& stats;
			Check check;
			Emitter emitter;
		};
	} // namespace slp
} // namespace rat

#endif
