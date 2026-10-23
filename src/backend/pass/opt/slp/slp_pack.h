// SLP vectorization on store chains Runs of adjacent stores and integer reductions pack into SSE vector

#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"
#include "pass/pass.h"

#include <tuple>

namespace rat {
	struct Node;
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
		U32 guardPairs = 0;			 // runtime disjointness checks emitted
		U32 rejected = 0;				 // windows rejected early, and seeds that did not pack
	};

	namespace slp {
		constexpr U32 kVecShift = 4;							 // log2 kVecBytes
		constexpr U32 kVecBytes = 1u << kVecShift; // SSE baseline
		constexpr U32 kMaxLanes = 4;							 // i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;							 // most inputs of a lane op, a select has 3
		constexpr U32 kNone = ~0u;								 // no record
		constexpr Opcode kNoOp = Opcode::Start;		 // no opcode
		constexpr I32 kMinProfit = 2;							 // scalar ops saved per window, net of lane plumbing
		constexpr U32 kMaxRunWindows = 4;					 // windows sharing one guard branch
		constexpr U32 kMaxGuards = 4;							 // runtime checks per guarded run
		constexpr I32 kGuardCheckCost = 6;				 // ops per runtime disjointness check
		constexpr I32 kGuardBranchCost = 6;				 // branch + else-arm bloat per guarded run
		constexpr U32 kMinTerms = 4;							 // fewest terms of a reduction
		constexpr U32 kMaxTerms = 64;							 // most terms of one reduction
		constexpr U32 kReadPiece = 4;							 // bytes of one entry of the reads index
		// offset bound of a known access, so its 16-byte blocks fit a block key
		constexpr I64 kMaxOffset = INT32_MAX - (I64)kVecBytes;
		// work bounds, so each search stays linear in the function
		constexpr U32 kMaxOps = 64;				// Op records tried per seed tuple
		constexpr U32 kMaxSetGroups = 16; // groups of a set, setDisjoint compares their bases pairwise
		constexpr U32 kMaxHops = 16;			// stores walked between a lane load's state and memIn

		// reduction finish shuffles by [w / 4][round], nibble i is the source of lane i
		constexpr U16 kFinishSel[2][2] = {{0x01}, {0x1032, 0x2301}};

		// lane type classes of the op table
		enum TypeBits : U8 { kI8 = 1, kI16 = 2, kI32 = 4, kI64 = 8, kF32 = 16, kF64 = 32, kPtr = 64 };
		constexpr U8 kI32I64 = kI32 | kI64, kFp = kF32 | kF64, kElem = kI32I64 | kFp | kPtr;

		// lanes of one opcode in [first, last] become one vector op of that opcode
		// a shift takes one amount for all lanes, a select takes a mask, every other input is a tuple
		struct OpRule {
			Opcode first, last;
			U8 types; // lane types, a compare lane is its mask
			U8 from;	// input 0 types of a compare or convert, 0 for no check
			B32 sse41;
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

		using Lanes = SmallList<Node*, kMaxLanes>;
		using Chain = List<StoreNode*>;

		// what loadcanon leaves for slp on the same function, to save compile time
		struct Handoff {
			const Function* fn = nullptr;
			U64 version = 0; // of fn when loadcanon finished
			List<Chain> chains;
			AddrCache addrs;
		};

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		struct Piece {
			MemMergeNode* set = nullptr;
			Chain chain;
		};

		struct Window {
			U32 begin = 0, w = 0, bytes = 0;
			I64 lo = 0;
			StoreNode* lane[kMaxLanes] = {};
		};

		// windows of one group that continue each other
		using Run = SmallList<Window, kMaxRunWindows>;

		// the tuples to pack, one vector each, for a run of stores or for a reduction
		struct Seed {
			List<Lanes> tuples;
			I32 profit = 0;
			U32 windows = 1;
			Node* memIn = nullptr;
			SmallList<StoreNode*, kMaxRunWindows * kMaxLanes> stores;
			SmallList<StoreNode*, kMaxRunWindows> heads;
			Node* root = nullptr;
			List<Node*> steps, rest;
			MemMergeNode* set = nullptr;
		};

		// how a record builds its vector
		enum class Kind : U8 {
			Gather,
			Load,
			Op,
		};

		// one vector of a plan
		struct Record {
			Lanes lanes;
			Kind kind = Kind::Gather;
			I32 profit = 0;
			Opcode op = kNoOp, op2 = kNoOp;
			U32 in[kMaxInputs] = {kNone, kNone, kNone};
			Node* amount = nullptr;
			Node* mem = nullptr;
			LoadNode* lowest = nullptr;
			I64 off = 0;
			I64 hi = 0; // end of the highest lane
			U32 wideLoads = 0;
			B32 shuffle = false;
			B32 guard = false;
			U16 sel = 0;
		};

		struct GuardGroup {
			U32 group = 0;
			Node* ptr = nullptr; // pointer of the access at lo
			I64 lo = 0, hi = 0;
		};

		struct Plan {
			List<Record> records;
			SmallList<U32, kMaxTerms / 4> tuples;
			List<GuardGroup> guards;
		};

		struct Made {
			List<Node*> nodes, vectors;
			List<Pair<LoadNode*, Addr>> loads;
		};

		struct Forms {
			Opcode op[kMaxLanes] = {};
			Node* in[kMaxLanes][kMaxInputs] = {};
			Opcode first = kNoOp, second = kNoOp;
		};

		enum Role : U8 { kLane = 1, kSink = 2, kKept = 4 };

		struct GuardSplit {
			Node *block, *memIn, *phi, *iff, *region; // phi: final state, iff: first guard branch
		};

		B32 guardsDisabled();
		B32 guardCostDisabled();

		// helpers without pass state
		U8 typeBits(const Type* t);
		void replaceUses(Node* from, Node* to);
		U64 pairKey(U32 high, I64 low);
		I32 gatherCost(const Lanes& lanes);
		B32 oneScalar(const Lanes& lanes);
		StoreNode* chainSuccessor(const StoreNode* prev);
		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n);
		List<Chain> collectChains(Function& fn);
		Handoff& handoff();
		Handoff takeHandoff(Function& fn);
		B32 setDisjoint(const List<Access>& acc, U32 lo, U32 hi, Map<U64, U16>& written);
		Node* lowerMerge(MemMergeNode* m);
		Seed runSeed(const Chain& chain, const Run& run);
		Seed setSeed(MemMergeNode* m, const Window& w);
		List<Node*> flattenReduction(Node* root, List<Node*>& steps);
		I32 profitMargin(const Seed& seed, const Plan& plan);
		B32 isInner(const Seed& seed, const Node* m);

		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats);
			U32 run();
			B32 pack(const Seed& seed);
			Type* laneType(const Node* n);
			// sets
			List<Piece> mergeStretches(const List<Chain>& chains);
			MemMergeNode* mergeStretch(const Chain& chain, U32 lo, U32 hi);
			// seed
			B32 windowAt(const Chain& chain, U32 at, U32 bytes, Window& out);
			U32 processChain(const Chain& chain);
			U32 processSet(MemMergeNode* m);
			U32 processRun(MemMergeNode* m, const Chain& run);
			U32 trySetWindow(MemMergeNode* m, const Chain& run, U32 p, U32 bytes);
			U32 processSeams();
			B32 splitBySeam(const Chain& chain, U32 i, U32 bytes);
			U32 tryRun(const Chain& chain, U32 i, U32 bytes);
			void extendRun(const Chain& chain, Run& run);
			I32 leafProfit(const Window& w);
			U32 packReductions();
			B32 isReductionRoot(const Node* n);
			U32 tryReduction(Node* root);
			// planner queries
			B32 bytesRead(U32 group, I64 off, U32 size, const Node* ctrl);
			void indexReads();
			// check
			void setRoles(const Seed& seed, const Plan& plan, B32 on);
			U8 roleOf(const Node* n) const;
			U32 keepLanes(const Plan& plan);
			B32 usedOutside(const Node* n, B32 copies) const;
			B32 dies(const Node* n) const;
			B32 innerReadersDie(const Seed& seed) const;
			// apply
			void apply(const Seed& seed, const Plan& plan);
			Made emit(const Plan& plan);
			Node* emitGather(const Record& r, Type* vecTy);
			Node* emitLoad(const Record& r, Type* vecTy, Made& made);
			Node* wideLoad(const Record& r, Type* vecTy, I64 off, Made& made);
			Node* emitOp(const Record& r, Type* vecTy, const Made& made);
			Node* makeOp(Opcode op, Type* vecTy, Node* const* in);
			void seedAnchors(const Seed& seed, const Plan& plan);
			Node* anchorPtr(Node* ptr, I64 off);
			Pair<Node*, I64> anchorOf(Node* ptr);
			Node* chainWideStores(const Seed& seed, const Made& made, Node* ctrl);
			void commitRun(const Seed& seed, const Made& made);
			void commitSet(const Seed& seed, const Made& made);
			void lowerSets(const List<Piece>& pieces);
			void renameMemIn(Node* from, Node* to);
			void commitGuarded(const Seed& seed, const Plan& plan, const Made& made);
			void moveToElse(const Seed& seed, const Plan& plan, Node* block, Node* elseP);
			Node* emitGuardCascade(const Seed& seed, const Plan& plan, Node*& iff, Node*& elseP);
			void splitBlocks();
			B32 moveLaterUsers(const GuardSplit& g, const Map<const Node*, U32>& runAt);
			void commitReduction(const Seed& seed, const Made& made);

			Function& fn;
			U32 ptrBytes;
			B32 sse41;
			SlpStats& stats;
			AddrCache addrs;
			Map<U32, Pair<Node*, I64>> anchors; // group -> (anchor ptr, its offset)
			Map<U64, Set<I64>> reads; // (control id, group) -> offsets of 4-byte pieces loads read
			B32 readsIndexed = false;
			List<GuardSplit> splits;				// the guarded runs, for splitBlocks
			Set<const Node*> guardedBlocks; // blocks with a guarded run, for loadState
			List<U8> roles;									// node id -> Role bits of the plan being checked, else 0
			Map<const Node*, U32> setSlot;	// member of a set -> its input slot
			Set<const Node*> seams;					// the last store of each piece, where a stretch ends
		};

		struct Planner {
			Planner(Slp& s, const Seed& seed);
			B32 planSeed();
			U32 vectorOf(const Lanes& lanes);
			U32 gather(const Lanes& lanes);
			// Load
			U32 loads(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Record& r);
			B32 permuteSpan(const Lanes& lanes, Record& r);
			B32 loadState(const Lanes& lanes, Record& r);
			B32 readsAtMemIn(LoadNode* l, const Addr& run, Record& r);
			Node* stepOverSet(MemMergeNode* m, LoadNode* l, U32& hops);
			// Op
			U32 op(const Lanes& lanes);
			B32 laneForms(const Lanes& lanes, Forms& f);
			void subAsAdd(const Lanes& lanes, Forms& f);
			B32 formRule(const Forms& f, Node* n0);
			B32 ruleOf(Opcode op, Node* n);
			U32 operandTuples(const Lanes& lanes, const Forms& f, Lanes* ops);
			B32 uniformShift(const Lanes& lanes);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orient(const Forms& f, Lanes* ops);
			I32 blendProfit(const Forms& f, U32 w, U16& sel);
			void addGuard(const Record& r);

			Slp& s;
			const Seed& seed;
			Plan plan;
			U32 budget = 0; // Op records left to try, kMaxOps more per seed tuple
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
