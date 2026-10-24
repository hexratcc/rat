// SLP internals by stage. Slp::run drives the stages on one function, Context holds what they share
// 1. chains, slp_chain.cpp, Slp: windows of stores along a chain or a set make one Seed per run
// 2. plan, slp_plan.cpp and slp_load.cpp, Plan: the vectors and runtime checks of a Seed
// 3. check, slp_pack.cpp, Check: the profit covers the kept lanes, and the inner readers die
// 4. apply, slp_apply.cpp, Emitter: the vectors, then the wide stores, guarded when a lane load
//    needs it, and after all chains the guarded blocks split
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
		constexpr U32 kVecShift = 4;							 // log2 kVecBytes
		constexpr U32 kVecBytes = 1u << kVecShift; // SSE baseline
		constexpr U32 kMaxLanes = 4;							 // i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;							 // most inputs of a lane op, a select has 3
		constexpr U32 kNoVector = ~0u;						 // no plan vector
		constexpr Opcode kNoOp = Opcode::Start;		 // no opcode
		constexpr I32 kMinProfit = 2;							 // least scalar ops saved per window or reduction
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
		constexpr U32 kMaxHops = 16;			// states walked between a load and memIn, or to its writers

		// lane type classes of the op table
		enum TypeBits : U8 { kI8 = 1, kI16 = 2, kI32 = 4, kI64 = 8, kF32 = 16, kF64 = 32, kPtr = 64 };
		constexpr U8 kI32I64 = kI32 | kI64;
		constexpr U8 kFp = kF32 | kF64;
		constexpr U8 kElem = kI32I64 | kFp | kPtr;

		using Lanes = SmallList<Node*, kMaxLanes>; // a tuple, one scalar per lane, all of one type
		// the stores of a chain or of a part of it in chain order, or a set's consecutive stores
		using Chain = List<StoreNode*>;

		// type classes and keys
		U8 typeBits(const Type* t);
		U64 pairKey(U32 high, I64 low);
		I64 blockEnd(I64 from, I64 end);

		// the function, the target and the tables the stages share. A lazy index is built from fn at
		// its first bytesRead or storesTo and never updated, so that moment shapes the output
		struct Context {
			Context(Function& fn, const TargetInfo& target);
			Type* laneType(const Node* n);
			B32 bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl); // slp_load.cpp
			B32 storesTo(U32 object);																					// slp_load.cpp
			U32 guardedRunsIn(const Node* block) const;

			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			AddrCache addrs;													 // Slp::run takes it from the loadcanon handoff
			Map<const Node*, U32> guardedRunsPerBlock; // block -> guarded runs, commitGuarded adds
		private:
			void indexReads();
			void indexStores();
		private:
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of 4-byte pieces loads read
			B32 readsIndexed = false;
			Set<U32> storedObjects; // objects the stores of fn write
			B32 storesIndexed = false;
		};

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
			I32 profit = 0;			// scalar ops saved besides the plan's vectors
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

		// the lanes of an Op vector as one opcode each, with their operand tuples
		struct LaneOps {
			Opcode opcode[kMaxLanes] = {}; // Neg as Sub from 0, Not as Xor with -1
			Lanes operands[kMaxInputs];		 // tuple k holds input k of each lane
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp;
		};

		I32 gatherCost(const Lanes& lanes, B32 reduction);
		B32 isSplat(const Lanes& lanes); // every lane is one node that is not a constant
		B32 pickOpcodes(LaneOps& ops, U32 laneCount);
		B32 isUniformShift(const Lanes& lanes);
		I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel);

		// the vectors of one seed and the runtime checks they need, apply sets Vector::node and loads
		struct Plan {
			Plan(Context& cx, const Seed& seed);
			B32 build();
			Node* tupleNode(U32 tuple) const;

			const Seed& seed;
			List<Vector> vectors;												// operands first
			SmallList<U32, kMaxTerms / 4> tupleVectors; // vector of each seed tuple
			List<GuardRange> guards;										// one runtime check each
			// the loads apply emitted with their address, loadAt reuses them, commitGuarded moves them
			List<Pair<LoadNode*, Addr>> loads;
		private:
			U32 planVector(const Lanes& lanes);
			U32 planGather(const Lanes& lanes);
			// Op
			U32 planOp(const Lanes& lanes);
			B32 readLaneOps(const Lanes& lanes, LaneOps& ops);
			B32 hasRuleForOps(const LaneOps& ops, Node* lane0);
			B32 hasRule(Opcode op, Node* lane);
			U32 operandTuples(const Lanes& lanes, LaneOps& ops);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orientOperands(LaneOps& ops);
			// Load, slp_load.cpp
			U32 planLoad(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Vector& vec);
			B32 permuteSpan(const Lanes& lanes, Vector& vec);
			B32 loadState(const Lanes& lanes, Vector& vec);
			B32 canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard);
			B32 forwardBlocked(const Vector& vec);
			B32 splitsLoad(Node* state, const Access& load, SmallList<Node*, kMaxHops>& walk);
			B32 callWrites(const CallNode* call, const Addr& addr);
			void addGuard(const Vector& vec);
		private:
			Context& cx;
			U32 budget = 0; // Op vectors left to try, kMaxOps more per seed tuple
		};

		// check
		I32 profitMargin(const Plan& plan);

		// the role of each node in the plan under check and apply, and the queries on it
		struct Check {
			explicit Check(const Function& fn);
			void setRoles(const Plan& plan, B32 on);
			U32 markKeptLanes(const Plan& plan);
			B32 laneDies(const Node* n) const;
			B32 innerReadersDie(const Seed& seed) const;
		private:
			U8 roleOf(const Node* n) const;
			B32 usedOutside(const Node* n, B32 copies) const;
		private:
			const Function& fn;
			// node id -> Role of slp_pack.cpp or 0, set for the check and apply of one plan
			List<U8> roles;
		};

		// apply

		// a run behind runtime guards, its block splits after all chains
		struct GuardedRun {
			Node* block = nullptr;
			StoreNode* firstStore = nullptr; // of the run, its memory follows every rewire of memIn
			PhiNode* phi = nullptr;					 // final state, input 0 is the region that merges the arms
			Node* firstIf = nullptr;				 // first guard branch
		};

		// emits an applied plan at addresses off one anchor per (control, group): its vectors, then its
		// wide stores, guarded or not, or its reduction. splitBlocks runs after all chains
		struct Emitter {
			explicit Emitter(Context& cx);
			void seedAnchors(const Plan& plan);
			void emitVectors(Plan& plan);
			void commitRun(const Plan& plan);
			void commitGuarded(const Plan& plan, const Check& check);
			void commitReduction(const Plan& plan); // slp_reduce.cpp
			void splitBlocks();
		private:
			Node* emitGather(const Vector& vec, Type* vecTy);
			Node* emitLoad(const Vector& vec, Type* vecTy, Plan& plan);
			Node* loadAt(const Vector& vec, Type* ty, I64 offset, Plan& plan);
			Node* emitOp(const Vector& vec, Type* vecTy, const Plan& plan);
			Node* makeOp(Opcode op, Type* vecTy, Node* const* inputs);
			Node* emitWideStores(const Plan& plan, Node* ctrl);
			Node* anchorPtr(Node* ptr, I64 offset, const Node* ctrl);
			Pair<Node*, I64> anchorOf(Node* ptr);
			// guarded runs
			void moveToElse(const Plan& plan, Node* block, Node* elseP, const Check& check);
			Node* emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP);
			B32 moveLaterUsers(const GuardedRun& guarded, const Map<const Node*, U32>& runAt);
		private:
			Context& cx;
			// (control, group) -> (anchor, its offset), the first applied plan that uses a key sets it
			Map<U64, Pair<Node*, I64>> anchors;
			List<GuardedRun> guardedRuns; // commitGuarded adds one, splitBlocks reads them
		};

		// the pass on one function, it searches the seeds and packs each through the stage objects
		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats);
			// the stages hold references to cx
			Slp(const Slp&) = delete;
			Slp& operator=(const Slp&) = delete;
			U32 run();
		private:
			B32 pack(const Seed& seed);
			void apply(Plan& plan); // slp_apply.cpp
			// chains and sets, slp_chain.cpp
			U32 packChains(const List<Chain>& chains, List<B32>& packed);
			B32 isDisjointSet(const Chain& chain, U32 first, U32 last, Map<U64, U16>& written);
			MemMergeNode* mergeStretch(const Chain& chain, U32 first, U32 last);
			B32 windowAt(const Chain& chain, U32 start, U32 bytes, Window& window);
			U32 packWindows(const Chain& chain);
			U32 packSet(const Chain& chain, U32 first, U32 last);
			U32 packRunAt(const Chain& chain, U32 start, U32 bytes);
			// reductions, slp_reduce.cpp
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 packReduction(Node* root);
		private:
			Context cx;
			SlpStats& stats; // the search and pack count windows and rejects, apply counts packs
			// the stages, pack runs a seed through them
			Check check;		 // pack checks a plan, Emitter::commitGuarded asks which lanes die
			Emitter emitter; // apply emits a plan, run splits the guarded blocks
		};
	} // namespace slp
} // namespace rat

#endif
