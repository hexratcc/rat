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

		// lanes of one opcode in [first, last] become one vector op of that opcode
		// a shift takes one amount for all lanes, a select takes a mask, every other input is a tuple
		struct OpRule {
			Opcode first;
			Opcode last;
			U8 laneTypes;	 // a compare lane is its mask
			U8 inputTypes; // of input 0 of a compare or convert, 0 for no check
			B32 needsSse41;
		};

		// clang-format off
		constexpr OpRule kOpRules[] = {
			{Opcode::Add, Opcode::Sub, kI32I64, 0, false},
			{Opcode::Mul, Opcode::Mul, kI32, 0, true},							// pmulld, no i64 mul
			{Opcode::And, Opcode::Xor, kI32I64, 0, false},
			{Opcode::Shl, Opcode::LShr, kI32I64, 0, false},
			{Opcode::AShr, Opcode::AShr, kI32, 0, false},						// psrad, no i64 sar
			{Opcode::FAdd, Opcode::FDiv, kFp, 0, false},
			{Opcode::FNeg, Opcode::FNeg, kFp, 0, false},
			{Opcode::Eq, Opcode::Ne, kI32I64, kI32I64, false},			// pcmpeqd, pcmpeqq
			{Opcode::Slt, Opcode::Ule, kI32, kI32, false},					// pcmpgtd, pminud
			{Opcode::FEq, Opcode::FGe, kI32I64, kFp, false},				// cmpps, cmppd
			{Opcode::SExt, Opcode::ZExt, kI32I64, kI8 | kI16 | kI32, true}, // pmovsx/zx
			{Opcode::SIToFP, Opcode::SIToFP, kFp, kI32, false},			// cvtdq2ps/pd
			{Opcode::FPToSI, Opcode::FPToSI, kI32, kFp, false},			// cvttps2dq/pd2dq
			{Opcode::FPExt, Opcode::FPExt, kF64, kF32, false},			// cvtps2pd
			{Opcode::FPTrunc, Opcode::FPTrunc, kF32, kF64, false},	// cvtpd2ps
			{Opcode::Select, Opcode::Select, kI32I64, 0, true},			// blendv
		};
		static_assert((I32)Opcode::Sub - (I32)Opcode::Add == 1 &&
									(I32)Opcode::Xor - (I32)Opcode::And == 2 &&
									(I32)Opcode::LShr - (I32)Opcode::Shl == 1 &&
									(I32)Opcode::FDiv - (I32)Opcode::FAdd == 3 &&
									(I32)Opcode::Ne - (I32)Opcode::Eq == 1 &&
									(I32)Opcode::Ule - (I32)Opcode::Slt == 3 &&
									(I32)Opcode::FGe - (I32)Opcode::FEq == 5 &&
									(I32)Opcode::ZExt - (I32)Opcode::SExt == 1,
									"kOpRules ranges follow ir/opcode.h");
		// clang-format on

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

		// laneCount stores of group from chain[start] fill bytes from offset, lane k is laneStore[k]
		struct Window {
			U32 start = 0;
			U32 laneCount = 0;
			U32 bytes = 0;
			U32 group = 0;
			I64 offset = 0;
			StoreNode* laneStore[kMaxLanes] = {};
		};

		using Run = SmallList<Window, kMaxRunWindows>; // consecutive windows of one group

		// the tuples to pack for a run of stores or a reduction
		struct Seed {
			List<Lanes> tuples; // one vector each
			I32 profit = 0;			// scalar ops saved besides the plan's vectors
			U32 windows = 1;		// each window must save kMinProfit
			// a run of stores
			Node* memIn = nullptr;																		// state before the run
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
		};

		struct GuardRange {
			U32 group = 0;
			Node* ptr = nullptr; // pointer of the access at begin
			I64 begin = 0;
			I64 end = 0;
		};

		struct Plan {
			List<Vector> vectors;												// operands first
			SmallList<U32, kMaxTerms / 4> tupleVectors; // vector of each seed tuple
			List<GuardRange> guards;										// one runtime check each
		};

		struct LaneOps {
			Opcode opcode[kMaxLanes] = {}; // Neg as Sub from 0, Not as Xor with -1
			Node* input[kMaxLanes][kMaxInputs] = {};
			Opcode mainOp = kNoOp;
			Opcode altOp = kNoOp;
		};

		struct Emitted {
			List<Node*> nodes;								 // one per plan vector
			List<Node*> tupleNodes;						 // one per seed tuple
			List<Pair<LoadNode*, Addr>> loads; // with their address, so loadAt reuses an equal load
		};

		struct GuardSplit {
			Node* block;
			StoreNode* firstStore; // of the run, its memory follows every rewire of memIn
			Node* phi;						 // final state
			Node* firstIf;				 // first guard branch
			Node* region;
		};

		// kLane a Load or Op lane, kSink a run store or reduction step, kKept a lane left scalar
		enum Role : U8 { kLane = 1, kSink = 2, kKept = 4 };

		// chains, sets and seeds
		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		StoreNode* chainSuccessor(const StoreNode* prev);
		List<Chain> collectChains(Function& fn);
		B32 isDisjointSet(const List<Access>& accesses, U32 first, U32 last, Map<U64, U16>& written);
		void lowerSet(MemMergeNode* set);
		void unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed);
		Seed seedOfRun(const Chain& chain, const Run& run, MemMergeNode* set);
		List<Node*> flattenReduction(Node* root, List<Node*>& steps);
		// type classes, keys and costs
		U8 typeBits(const Type* t);
		U64 pairKey(U32 high, I64 low);
		I32 gatherCost(const Lanes& lanes);
		B32 isSplat(const Lanes& lanes);
		I32 profitMargin(const Seed& seed, const Plan& plan);
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
			void extendRun(const Chain& chain, Run& run);
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 packReduction(Node* root);
			// planner queries
			B32 bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl);
			void indexReads();
			B32 storesTo(U32 object);
			void indexStores();
			// check
			void setRoles(const Seed& seed, const Plan& plan, B32 on);
			U8 roleOf(const Node* n) const;
			U32 markKeptLanes(const Plan& plan);
			B32 usedOutside(const Node* n, B32 copies) const;
			B32 laneDies(const Node* n) const;
			B32 innerReadersDie(const Seed& seed) const;
			// apply
			void apply(const Seed& seed, const Plan& plan);
			Emitted emitPlan(const Plan& plan);
			Node* emitGather(const Vector& vec, Type* vecTy);
			Node* emitLoad(const Vector& vec, Type* vecTy, Emitted& emitted);
			Node* loadAt(const Vector& vec, Type* ty, I64 offset, Emitted& emitted);
			Node* emitOp(const Vector& vec, Type* vecTy, const Emitted& emitted);
			Node* makeOp(Opcode op, Type* vecTy, Node* const* inputs);
			void seedAnchors(const Seed& seed, const Plan& plan);
			Node* anchorPtr(Node* ptr, I64 offset);
			Pair<Node*, I64> anchorOf(Node* ptr);
			Node* emitWideStores(const Seed& seed, const Emitted& emitted, Node* ctrl);
			void commitRun(const Seed& seed, const Emitted& emitted);
			void commitGuarded(const Seed& seed, const Plan& plan, const Emitted& emitted);
			void moveToElse(const Seed& seed, const Plan& plan, Node* block, Node* elseP);
			Node* emitGuardCascade(const Seed& seed, const Plan& plan, Node*& firstIf, Node*& elseP);
			void splitBlocks();
			B32 moveLaterUsers(const GuardSplit& split, const Map<const Node*, U32>& runAt);
			void commitReduction(const Seed& seed, const Emitted& emitted);

			// function, target and stats
			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			SlpStats& stats;
			// address facts
			AddrCache addrs;
			Map<U32, Pair<Node*, I64>> anchors; // group -> (anchor ptr, its offset)
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of 4-byte pieces loads read
			B32 readsIndexed = false;
			Set<U32> storedObjects; // objects the stores of fn write
			B32 storesIndexed = false;
			// guarded runs
			List<GuardSplit> splits;				// for splitBlocks
			Set<const Node*> guardedBlocks; // their blocks, for loadState
			// plan check
			List<U8> roles; // node id -> Role bits, else 0
		};

		struct Planner {
			Planner(Slp& slp, const Seed& seed);
			B32 planSeed();
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
			B32 isUniformShift(const Lanes& lanes);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orientOperands(const LaneOps& ops, Lanes* tuples);
			I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel);

			Slp& slp;
			const Seed& seed;
			Plan plan;
			U32 budget = 0; // Op vectors left to try, kMaxOps more per seed tuple
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
