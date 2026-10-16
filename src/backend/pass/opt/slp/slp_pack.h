// SLP vectorization on store chains Runs of adjacent stores and integer reductions pack into SSE vector

#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "core.h"

#include "analysis/address.h"
#include "ir/opcode.h"
#include "pass/pass.h"

#include <array>
#include <tuple>

namespace rat {
	struct Node;
	struct StoreNode;
	struct LoadNode;
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
		constexpr U32 kVecBytes = 16;						// SSE baseline
		constexpr U32 kVecShift = 4;						// log2 kVecBytes
		constexpr U32 kMaxLanes = 4;						// i32/f32 in 16 bytes
		constexpr U32 kMaxInputs = 3;						// most inputs of a lane op, a select has 3
		constexpr U32 kNone = ~0u;							// no record
		constexpr Opcode kNoOp = Opcode::Start; // no opcode
		constexpr I32 kMinProfit = 2;						// scalar ops saved per window, net of lane plumbing
		constexpr U32 kMaxRunWindows = 4;				// windows sharing one guard branch
		constexpr U32 kMaxGuards = 4;						// runtime checks per guarded run
		constexpr I32 kGuardCheckCost = 6;			// ops per runtime disjointness check
		constexpr I32 kGuardBranchCost = 6;			// branch + else-arm bloat per guarded run
		constexpr U32 kMaxTerms = 64;						// most terms of one reduction
		static_assert(1u << kVecShift == kVecBytes, "kVecShift is log2 kVecBytes");
		// offset bound of a known access, so its 16-byte blocks fit a block key
		constexpr I64 kMaxOffset = INT32_MAX - (I64)kVecBytes;
		// work bounds, so each search stays linear in the function
		constexpr U32 kMaxOps = 64;				 // Op records tried per seed tuple
		constexpr U32 kMaxSortGroups = 16; // groups of a stretch, the sort compares them pairwise
		constexpr U32 kMaxCone = 64;			 // address cone nodes visited per hoisted load
		constexpr U32 kMaxHops = 16;			 // stores walked between a lane load's state and memIn

		// reduction finish shuffles by [w / 4][round], nibble i is the source of lane i
		constexpr U16 kFinishSel[2][2] = {{0x01}, {0x1032, 0x2301}};

		// lane type classes of the op table
		enum TypeBits : U8 { kI8 = 1, kI16 = 2, kI32 = 4, kI64 = 8, kF32 = 16, kF64 = 32, kPtr = 64 };
		constexpr U8 kI32I64 = kI32 | kI64, kFp = kF32 | kF64, kElem = kI32I64 | kFp;

		// how the inputs of op lanes become the operands of their vector op
		enum class Form : U8 {
			Lanewise, // every input is a tuple
		};

		// lanes of one opcode in [first, last] become one vector op of that opcode
		struct OpRule {
			Opcode first, last;
			U8 types; // lane types, a compare lane is its mask
			U8 from;	// input 0 types of a compare or convert, 0 for no check
			Form form;
			B32 sse41;
		};

		// clang-format off
		constexpr OpRule kOpRules[] = {
			{Opcode::Add, Opcode::Sub, kI32I64, 0, Form::Lanewise, false},
			{Opcode::Mul, Opcode::Mul, kI32, 0, Form::Lanewise, true},						// pmulld, no i64 mul
			{Opcode::And, Opcode::Xor, kI32I64, 0, Form::Lanewise, false},
			{Opcode::FAdd, Opcode::FDiv, kFp, 0, Form::Lanewise, false},
		};
		// clang-format on
		static_assert((I32)Opcode::Sub - (I32)Opcode::Add == 1 &&
											(I32)Opcode::Xor - (I32)Opcode::And == 2 &&
											(I32)Opcode::FDiv - (I32)Opcode::FAdd == 3,
									"kOpRules ranges follow ir/opcode.h");

		using Lanes = SmallList<Node*, kMaxLanes>;
		using Chain = List<StoreNode*>;

		struct Access {
			Addr addr;
			U32 size = 0;
		};

		struct LastOutside {
			void add(U32 at, U32 group);
			U32 outside(U32 group) const; // the last store outside group, 0 for none

			U32 last = 0, lastGroup = 0, lastOther = 0;
		};

		// the stores of a chain seen so far (indexed from 1), to find the last that may overlap a load
		struct Conflicts {
			static U64 blockKey(U32 group, I64 offset);
			void add(U32 at, const Access& a);
			U32 lastConflict(const Access& a) const;

			LastOutside all;														 // every store
			U32 lastWild = 0;														 // the last store without an object
			Map<U32, LastOutside> objects;							 // object -> its stores
			Map<U64, std::array<U32, kVecBytes>> blocks; // (group, 16-byte block) -> last store per byte
		};

		// sort key of a store in a stretch
		struct SortKey {
			B32 operator<(const SortKey& o) const;

			U32 groupAt = 0;
			I64 offset = 0;
			U32 at = 0;
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
			List<Node*> steps;
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
			Opcode op = kNoOp;
			U32 in[kMaxInputs] = {kNone, kNone, kNone};
			Node* mem = nullptr;
			LoadNode* lowest = nullptr;
			I64 off = 0;
			U32 wideLoads = 0;
			B32 guard = false;
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
			Opcode first = kNoOp;
		};

		// whether a lane load may read memIn instead of its state
		enum class Edge : U8 { None, Weak, Strong };

		enum Role : U8 { kLoadLane = 1, kOpLane = 2, kSink = 4, kKept = 8 };

		struct GuardSplit {
			Node *block, *memIn, *phi, *iff, *region; // phi: final state, iff: first guard branch
		};

		B32 guardsDisabled();
		B32 guardCostDisabled();
		B32 statsEnabled();

		// helpers without pass state
		U8 typeBits(const Type* t);
		Node* memoryOperand(const Node* n);
		void replaceUses(Node* from, Node* to);
		I32 gatherCost(const Lanes& lanes);
		B32 oneScalar(const Lanes& lanes);
		StoreNode* chainSuccessor(StoreNode* prev);
		U32 coneState(LoadNode* l, const List<U32>& index);
		void sortStretch(Chain& chain, const List<Access>& acc, U32 lo, U32 hi);
		B32 sortDisjoint(const List<Access>& acc, const List<SortKey>& keys);
		Seed runSeed(const Chain& chain, const Run& run);
		List<Node*> flattenReduction(Node* root, List<Node*>& steps);
		I32 profitMargin(const Seed& seed, const Plan& plan);
		B32 isInner(const Seed& seed, const Node* m);

		struct Slp {
			Slp(Function& fn, const TargetInfo& target, SlpStats& stats);
			U32 run();
			B32 pack(const Seed& seed);
			// order
			Access accessOf(Node* n);
			List<Chain> collectChains();
			void orderChain(Chain& chain, List<U32>& index);
			void hoistLoads(const Chain& chain, const List<Access>& acc, List<U32>& index);
			// seed
			B32 windowAt(const Chain& chain, U32 at, U32 bytes, Window& out);
			U32 processChain(const Chain& chain);
			U32 tryRun(const Chain& chain, U32 i, U32 bytes);
			void extendRun(const Chain& chain, Run& run);
			I32 leafProfit(const Window& w);
			U32 packReductions();
			B32 reduces(const Node* n);
			U32 tryReduction(Node* root);
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
			Node* anchorPtr(Node* ptr, I64 off);
			Pair<Node*, I64> anchorOf(Node* ptr);
			Node* chainWideStores(const Seed& seed, const Made& made, Node* ctrl);
			void commitRun(const Seed& seed, const Made& made);
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
			List<GuardSplit> splits;						// the guarded runs, for splitBlocks
			Set<const Node*> guardedBlocks;			// blocks with a guarded run, for loadState
			List<U8> roles;											// node id -> Role bits of the plan being checked, else 0
		};

		struct Planner {
			Planner(Slp& s, const Seed& seed);
			B32 planSeed();
			U32 vectorOf(const Lanes& lanes);
			U32 gather(const Lanes& lanes);
			// Load
			U32 loads(const Lanes& lanes);
			B32 loadShape(const Lanes& lanes, Record& r);
			B32 loadState(const Lanes& lanes, Record& r);
			Edge readsAtMemIn(LoadNode* l, const Addr& run);
			// Op
			U32 op(const Lanes& lanes);
			B32 laneForms(const Lanes& lanes, Forms& f);
			const OpRule* ruleOf(Opcode op, Node* n);
			U32 operandTuples(const Lanes& lanes, const Forms& f, Lanes* ops);
			std::tuple<Opcode, const Type*, U32> operandKey(Node* n);
			void orient(const Forms& f, Lanes* ops);
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
		static B32 hasVolatileAccess(Function& fn);

		SlpStats stats;
	};
} // namespace rat

#endif
