#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "core.h"

#include "ir/opcode.h"
#include "pass/opt/slp/slp_util.h"
#include "pass/pass.h"

namespace rat {
	struct Node;
	struct StoreNode;
	struct LoadNode;
	struct BinaryNode;
	struct Type;
	struct AliasAnalysis;

	struct SlpStats {
		U32 windowsSeen = 0;		 // structurally valid windows examined
		U32 packedUnguarded = 0; // windows fused statically
		U32 packedGuarded = 0;	 // windows fused behind a runtime guard
		U32 packedReduction = 0; // add chains folded into vector hsum
		U32 guardedRuns = 0;		 // merged guard regions emitted
		U32 guardPairs = 0;			 // runtime disjointness checks emitted
		U32 rejectedTree = 0;		 // vector recomputation failed to build
		U32 rejectedProfit = 0;	 // cost model said no
		U32 rejectedGuarded = 0; // guarded gates (budget/escape/contamination)
		U32 packSplat = 0;
		U32 packConst = 0;
		U32 packWideLoad = 0;
		U32 splatGrouped = 0; // splat sources folded into a shared wide load
		U32 packBinary = 0;
		U32 packFrontier = 0;
		U32 orientSwaps = 0; // commutative lanes reordered by shape key
		U32 memoHits = 0;		 // shared subtrees packed once
	};

	struct SlpPackPass : FunctionPass {
		using RefinedAddr = slp::RefinedAddr;
		using ShapeHash = slp::ShapeHash;

		static constexpr U32 kMaxDepth = 16;			 // pack-growth recursion bound
		static constexpr I32 kMinProfit = 2;			 // scalar ops saved, net of lane plumbing
		static constexpr U32 kMaxRunWindows = 4;	 // windows sharing one guard branch
		static constexpr U32 kMaxGuards = 4;			 // runtime checks per guarded run
		static constexpr I32 kGuardCheckCost = 6;	 // ops per runtime disjointness check
		static constexpr I32 kGuardBranchCost = 6; // branch + else-arm bloat per guarded run

		struct StoreInfo {
			StoreNode* store = nullptr;
			RefinedAddr key;
			List<LoadNode*> observers; // loads reading this store's output state
		};

		// one walk of the store chain through candidate stores
		using Segment = List<StoreInfo>;

		struct WindowShape {
			U32 begin = 0;
			U32 w = 0;
			U32 esz = 0;
			Type* elemTy = nullptr;
			Node* ctrl = nullptr;
			I64 lo = 0;
			List<const StoreInfo*> byOff; // lane order
		};

		// grows one vector value per store window
		struct Packer {
			Function& fn;
			const AliasAnalysis& aa;
			U32 ptrBytes;
			B32 sse41;
			ShapeHash& shapes;
			SlpStats& st;

			// window context
			Node* memIn = nullptr;
			const RefinedAddr* windowKey = nullptr;
			const Map<const Node*, List<I64>>* interWritten = nullptr;
			const Set<const Node*>* observers = nullptr;
			// group sig -> (anchor ptr, its refined constant), shared across windows
			Map<String, Pair<Node*, I64>>* addrAnchors = nullptr;

			// runtime disjointness guards
			struct GuardGroup {
				String sig;					 // group identity
				Node* ptr = nullptr; // pointer of the lane at minC
				I64 minC = 0, maxC = 0;
			};

			List<GuardGroup> guardGroups;

			// splat reloads kept for post-commit coalescing into wide loads
			List<Pair<Node*, LoadNode*>> splatLoads;

			// tuple -> already built vector node, so shared subtrees pack once
			Map<String, Node*> memo;
			I32 profit = 0;
			U32 interior = 0; // vector arithmetic / wide loads created

			Packer(Function& fn,
						 const AliasAnalysis& aa,
						 U32 ptrBytes,
						 B32 sse41,
						 ShapeHash& shapes,
						 SlpStats& st);

			void addGuard(const RefinedAddr& k, Node* lane0Ptr, U32 bytes);
			void bindWindow(Node* memIn,
											const RefinedAddr* windowKey,
											const Map<const Node*, List<I64>>* interWritten,
											const Set<const Node*>* observers,
											Map<String, Pair<Node*, I64>>* addrAnchors);
			Node* anchorPtr(Node* ptr, const RefinedAddr& k);
			void coalesceSplats();
			B32 coneTouchesObserver(const Node* n) const;
			static String tupleKey(const List<Node*>& lanes);
			Node* packTuple(const List<Node*>& lanes, Type* elemTy, U32 depth);
			Node* packTupleUncached(const List<Node*>& lanes, Type* elemTy, Type* vecTy, U32 depth);
			Node* packLoads(const List<Node*>& lanes, Type* elemTy, Type* vecTy, B32& hardFail);
			Node* packBinaryLanes(
					const List<Node*>& lanes, Type* elemTy, Type* vecTy, U32 depth, B32& hardFail);
			Node* packWideOrSplat(Node* mem,
														LoadNode* first,
														const RefinedAddr& k0,
														Type* elemTy,
														Type* vecTy,
														U32 w,
														B32 equal);
		};

		// function driver
		struct Slp {
			Function& fn;
			const AliasAnalysis& aa;
			U32 ptrBytes;
			B32 sse41;
			SlpStats& stats;
			ShapeHash shapes;
			Map<String, Pair<Node*, I64>> addrAnchors;
			Map<const Node*, RefinedAddr> keyCache; // store -> refined address (refineAddr is not free)
			Map<const Node*, String> sigCache;			// store -> group signature (groupSig builds a String)

			Slp(Function& fn, const AliasAnalysis& aa, U32 ptrBytes, B32 sse41, SlpStats& stats);

			void normalizeLoadEdges();
			Node* storeBaseObj(StoreNode* s, Map<const Node*, Node*>& cache);
			Node* skipDisjointRun(StoreNode* s,
														Node* loadBase,
														Map<const Node*, Node*>& storeBase,
														Map<const Node*, Map<const Node*, Node*>>& skipMemo);
			void normalizeStoreChains();
			B32 storeKey(StoreNode* s, RefinedAddr& out);
			const String& storeSig(StoreNode* s); // cached groupSig of a store's address
			B32 trySwapAdjacentStores(StoreNode* s, StoreNode* p);
			// thin fn.create wrappers for the guard cascade
			Node* guardBranch(Node* ctrl, Node* pred);
			Node* guardProj(Node* of, U32 index, const C8* label);
			Map<Node*, StoreInfo> collectCandidates();
			List<Segment> buildSegments(const Map<Node*, StoreInfo>& cand);
			U32 tryStaticWindow(Segment& seg, U32 i, const WindowShape& w0);
			U32 tryGuardedRun(Segment& seg, U32 i, const WindowShape& w0);
			B32
			observersConfined(const Segment& seg, U32 i, U32 total, const Set<const Node*>& obsSet) const;
			void commitGuardedRun(Segment& seg,
														U32 i,
														const List<WindowShape>& run,
														const List<Node*>& vecs,
														Packer& packer,
														const Map<const Node*, List<I64>>& interWritten);
			// build the disjointness-guard branch cascade; returns the speculating
			// then-control and fills iff (first branch) and fails (else-arm controls)
			Node* emitGuardCascade(const List<Packer::GuardGroup>& groups,
														 Node* startCtrl,
														 Node* wPtr,
														 Node* wEnd,
														 Node*& iff,
														 List<Node*>& fails);
			// states the incoming window state transitively derives from
			static void collectPreStates(Node* memIn, Set<const Node*>& preSet);
			// steer every other user of the pre-branch control to the right arm
			void rerouteBlock(Node* startCtrl,
												Node* iff,
												Node* region,
												Node* elseP,
												const Set<const Node*>& wideStores,
												const Set<const Node*>& runStores,
												const Set<const Node*>& preSet,
												const Map<const Node*, List<I64>>& interWritten);
			U32 processSegment(Segment& seg);
			U32 packReduction(BinaryNode* root);
			U32 packReductions();
			U32 run();
		};

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;

		// env-gated diagnostics/tuning
		static B32 statsEnabled();
		static B32 shapesDisabled();
		static B32 guardsDisabled();
		static B32 guardCostDisabled();
		static B32 fpReduceEnabled();

		// window/segment machinery over the pass-specific StoreInfo/Segment/WindowShape
		static StoreNode* soleChainSuccessor(StoreNode* s, List<LoadNode*>& observers);
		static B32 windowAt(const Segment& seg, U32 at, WindowShape& out);
		static B32 windowHasObs(const Segment& seg, const WindowShape& ws);
		static List<Node*> flattenAddChain(BinaryNode* root, Opcode addOp, Type* t);
		static List<Node*> laneValues(const WindowShape& w);
		static void collectInterState(const Segment& seg,
																	U32 begin,
																	U32 count,
																	Map<const Node*, List<I64>>& interWritten,
																	Set<const Node*>& obsSet);
	private:
		SlpStats stats;
	};
} // namespace rat

#endif
