// SLP internals in stage order. Slp::run drives the stages on one function:
// 1. chains, slp_chain.cpp: the store chains come from the loadcanon handoff, a disjoint stretch
//    of a chain becomes a set, and windows of stores along a chain or a set make one Seed per run
// 2. plan, slp_plan.cpp, slp_load.cpp and slp_guard.cpp: Plan::build covers each seed tuple with a
//    Gather, Load or Op vector, operands first, and adds the runtime checks its loads need
// 3. check, slp_pack.cpp: Slp::pack keeps a plan whose profit covers its kept lanes and whose inner
//    readers die
// 4. apply, slp_apply.cpp and slp_guard.cpp: emit the vectors, then the wide stores, behind a
//    runtime guard when a lane load needs one
// 5. after all chains, load_canon.cpp and slp_guard.cpp: unhoist the loads of chains without a
//    pack, then split the blocks of the guarded runs
// 6. reductions, slp_reduce.cpp: each reduction root is a seed and goes through stages 2 to 4

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
		constexpr U32 kVecShift = 4;							 // log2 kVecBytes
		constexpr U32 kVecBytes = 1u << kVecShift; // SSE baseline
		constexpr U32 kMaxLanes = 4;							 // i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;							 // most inputs of a lane op, a select has 3
		constexpr U32 kNoVector = ~0u;						 // no plan vector
		constexpr Opcode kNoOp = Opcode::Start;		 // no opcode
		constexpr I32 kMinProfit = 2;							 // scalar ops saved per window, net of lane plumbing
		constexpr U32 kMaxRunWindows = 4;					 // windows of one run, which share one guard
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
		constexpr U8 kI32I64 = kI32 | kI64;
		constexpr U8 kFp = kF32 | kF64;
		constexpr U8 kElem = kI32I64 | kFp | kPtr;

		using Lanes = SmallList<Node*, kMaxLanes>; // a tuple, one scalar per lane
		// the stores of a chain or of a part of it in chain order, or a set's consecutive stores
		using Chain = List<StoreNode*>;

		// type classes and keys
		U8 typeBits(const Type* t);
		U64 pairKey(U32 high, I64 low);
		I64 blockEnd(I64 from, I64 end);

		// chains, sets and seeds

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		// laneCount stores of group from chain[start] fill bytes from offset, lane k is laneStore[k]
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
			// a run of stores, the stores in chain order, in a set by offset
			SmallList<StoreNode*, kMaxRunWindows * kMaxLanes> stores;
			SmallList<StoreNode*, kMaxRunWindows> lowestStores; // the lane 0 store of each window
			// a reduction
			Node* root = nullptr;
			List<Node*> steps;			 // the ops the vectors replace
			List<Node*> scalarTerms; // terms left as scalars
		};

		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		B32 hasInnerReader(StoreNode* const* stores, U32 count);
		void addWindow(Seed& seed, const Chain& chain, const Window& window);
		B32 isDisjointSet(const List<Access>& accesses, U32 first, U32 last, Map<U64, U16>& written);
		void lowerSet(MemMergeNode* set);
		List<Node*> flattenReduction(Node* root, List<Node*>& steps);
		Node* memIn(const Seed& seed);
		B32 isInner(const Seed& seed, const Node* state);
		MemMergeNode* setOf(const Seed& seed);

		// plan

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

		// one runtime check, the loads of group read [begin, end) from ptr at begin
		struct GuardRange {
			U32 group = 0;
			Node* ptr = nullptr; // pointer of the access at begin
			I64 begin = 0;
			I64 end = 0;
		};

		// the lanes of an Op vector as one opcode each, with their inputs
		struct LaneOps {
			Opcode opcode[kMaxLanes] = {}; // Neg as Sub from 0, Not as Xor with -1
			Node* input[kMaxLanes][kMaxInputs] = {};
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp;
		};

		I32 gatherCost(const Lanes& lanes, B32 reduction);
		B32 isSplat(const Lanes& lanes); // every lane is one node that is not a constant
		B32 pickOpcodes(LaneOps& ops, U32 laneCount);
		B32 isUniformShift(const Lanes& lanes);
		I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel);

		struct Slp;

		// the vectors of one seed and the runtime checks they need, build writes them, the check reads
		// them, apply reads them and sets Vector::node and loads
		struct Plan {
			Plan(Slp& slp, const Seed& seed);
			B32 build();
			U32 planVector(const Lanes& lanes);
			U32 planGather(const Lanes& lanes);
			// Op
			U32 planOp(const Lanes& lanes);
			B32 readLaneOps(const Lanes& lanes, LaneOps& ops);
			B32 hasRuleForOps(const LaneOps& ops, Node* lane0);
			B32 hasRule(Opcode op, Node* lane);
			U32 operandTuples(const Lanes& lanes, const LaneOps& ops, Lanes* tuples);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orientOperands(const LaneOps& ops, Lanes* tuples);
			// Load, slp_load.cpp
			U32 planLoad(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Vector& vec);
			B32 permuteSpan(const Lanes& lanes, Vector& vec);
			B32 loadState(const Lanes& lanes, Vector& vec);
			B32 canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard);
			B32 forwardBlocked(const Vector& vec);
			B32 splitsLoad(Node* state, const Access& load, SmallList<Node*, kMaxStlfHops>& walk);
			B32 callWrites(const CallNode* call, const Addr& addr);
			// guards, slp_guard.cpp
			void addGuard(const Vector& vec);
			// apply, slp_apply.cpp
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

		// check
		I32 profitMargin(const Plan& plan);

		// apply

		// a run behind runtime guards, its block splits after all chains
		struct GuardedRun {
			Node* block = nullptr;
			StoreNode* firstStore = nullptr; // of the run, its memory follows every rewire of memIn
			PhiNode* phi = nullptr;					 // final state, input 0 is the region that merges the arms
			Node* firstIf = nullptr;				 // first guard branch
		};

		// the pass on one function, the state its stages share
		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats);
			U32 run();
			B32 pack(const Seed& seed);
			Type* laneType(const Node* n);
			// chains and sets, slp_chain.cpp
			U32 packChains(const List<Chain>& chains, List<B32>& packed);
			MemMergeNode* mergeStretch(const Chain& chain, U32 first, U32 last);
			B32 windowAt(const Chain& chain, U32 start, U32 bytes, Window& window);
			U32 packWindows(const Chain& chain);
			U32 packSet(MemMergeNode* set);
			U32 packRunAt(const Chain& chain, U32 start, U32 bytes);
			void extendRun(const Chain& chain, const Window& first, U32 bytes, Seed& seed);
			// reductions, slp_reduce.cpp
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 packReduction(Node* root);
			void commitReduction(const Plan& plan);
			// plan queries, slp_load.cpp
			B32 bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl);
			void indexReads();
			B32 storesTo(U32 object);
			void indexStores();
			// check, slp_pack.cpp
			void setRoles(const Plan& plan, B32 on);
			U8 roleOf(const Node* n) const;
			U32 markKeptLanes(const Plan& plan);
			B32 usedOutside(const Node* n, B32 copies) const;
			B32 laneDies(const Node* n) const;
			B32 innerReadersDie(const Seed& seed) const;
			// apply, slp_apply.cpp
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
			// guarded runs, slp_guard.cpp
			void commitGuarded(const Plan& plan);
			void moveToElse(const Plan& plan, Node* block, Node* elseP);
			Node* emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP);
			void splitBlocks();
			B32 moveLaterUsers(const GuardedRun& guarded, const Map<const Node*, U32>& runAt);

			// function and target, set once
			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			// packRunAt, extendRun and packReduction count windows, packRunAt and pack count rejects,
			// apply counts packs
			SlpStats& stats;
			// run takes addrs from the handoff, every stage reads it
			AddrCache addrs;
			// (control, group) -> (anchor ptr, its offset), seedAnchors adds a key the first time an applied
			// plan uses it and never changes it, anchorPtr reads it
			Map<U64, Pair<Node*, I64>> anchors;
			// lazy indexes, the first bytesRead or storesTo builds one from fn as the packs before left
			// it, a later pack does not update it, so the moment of the first query shapes the output
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of 4-byte pieces loads read
			B32 readsIndexed = false;
			Set<U32> storedObjects; // objects the stores of fn write
			B32 storesIndexed = false;
			// commitGuarded adds to both, Plan::build and loadState read the count, splitBlocks reads
			// the list after all chains
			List<GuardedRun> guardedRuns;
			Map<const Node*, U32> guardedRunsPerBlock; // block -> its guarded run count
			// node id -> Role of slp_pack.cpp, else 0, setRoles sets and clears it around the check and
			// apply of one plan, markKeptLanes clears kept lanes, usedOutside and laneDies read it
			List<U8> roles;
		};
	} // namespace slp
} // namespace rat

#endif
