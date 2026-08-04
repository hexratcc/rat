#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "Core.h"

#include "IR/Opcode.h"
#include "Pass/Pass.h"

namespace rat {
	struct Node;
	struct StoreNode;
	struct LoadNode;
	struct Type;
	struct AliasAnalysis;

	struct SlpStats {
		U32 windowsSeen = 0;		 // structurally valid windows examined
		U32 packedUnguarded = 0; // windows fused statically
		U32 packedGuarded = 0;	 // windows fused behind a runtime guard
		U32 guardedRuns = 0;		 // merged guard regions emitted
		U32 guardPairs = 0;			 // runtime disjointness checks emitted
		U32 rejectedTree = 0;		 // vector recomputation failed to build
		U32 rejectedProfit = 0;	 // cost model said no
		U32 rejectedGuarded = 0; // guarded gates (budget/escape/contamination)
		U32 packSplat = 0;
		U32 packConst = 0;
		U32 packWideLoad = 0;
		U32 packBinary = 0;
		U32 packFrontier = 0;
		U32 orientSwaps = 0; // commutative lanes reordered by shape key
		U32 memoHits = 0;		 // shared subtrees packed once
	};

	struct SlpPackPass : FunctionPass {
		static constexpr U32 kVecBytes = 16; // SSE baseline
		static constexpr U32 kMaxDepth = 8;	 // pack-growth recursion bound
		static constexpr I32 kMinProfit = 2; // scalar ops saved, net of lane plumbing

		struct RefinedAddr {
			Node* base = nullptr;
			I64 constant = 0;
			List<std::pair<const Node*, I64>> terms; // (var, scale), sorted by id
			U32 size = 0;														 // access bytes

			B32 valid() const { return base != nullptr && size != 0; }
			B32 sameGroup(const RefinedAddr& o) const {
				return base == o.base && size == o.size && terms == o.terms;
			}
		};

		// bounded-depth structural hashing
		struct ShapeHash {
			static constexpr U32 kDepth = 4;
			Map<const Node*, U64> memo; // at kDepth

			static U64 mix(U64 h, U64 v) {
				h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				return h;
			}

			U64 shape(const Node* n, U32 depth);
			U64 operator()(const Node* n) { return shape(n, kDepth); }
		};

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
			ShapeHash& shapes;
			SlpStats& st;

			// window context
			Node* memIn = nullptr;
			const RefinedAddr* windowKey = nullptr;
			const Map<const Node*, List<I64>>* interWritten = nullptr;
			const Set<const Node*>* observers = nullptr;

			// runtime disjointness guards
			struct GuardGroup {
				String sig;					 // group identity
				Node* ptr = nullptr; // pointer of the lane at minC
				I64 minC = 0, maxC = 0;
			};

			List<GuardGroup> guardGroups;

			// tuple -> already built vector node, so shared subtrees pack once
			Map<String, Node*> memo;
			I32 profit = 0;
			U32 interior = 0; // vector arithmetic / wide loads created

			Packer(Function& fn, const AliasAnalysis& aa, U32 ptrBytes, ShapeHash& shapes, SlpStats& st)
			: fn(fn),
				aa(aa),
				ptrBytes(ptrBytes),
				shapes(shapes),
				st(st) {}

			void addGuard(const RefinedAddr& k, Node* lane0Ptr, U32 bytes);
			B32 coneTouchesObserver(const Node* n) const;
			static String tupleKey(const List<Node*>& lanes);
			Node* packTuple(const List<Node*>& lanes, Type* elemTy, U32 depth);
			Node* packTupleUncached(const List<Node*>& lanes, Type* elemTy, Type* vecTy, U32 depth);
			Node* packLoads(const List<Node*>& lanes, Type* elemTy, Type* vecTy, B32& hardFail);
		};

		// function driver
		struct Slp {
			Function& fn;
			const AliasAnalysis& aa;
			U32 ptrBytes;
			SlpStats& stats;
			ShapeHash shapes;

			Slp(Function& fn, const AliasAnalysis& aa, U32 ptrBytes, SlpStats& stats)
			: fn(fn),
				aa(aa),
				ptrBytes(ptrBytes),
				stats(stats) {}

			void normalizeLoadEdges();
			void normalizeStoreChains();
			Map<Node*, StoreInfo> collectCandidates();
			List<Segment> buildSegments(const Map<Node*, StoreInfo>& cand);
			U32 tryStaticWindow(Segment& seg, U32 i, const WindowShape& w0);
			U32 processSegment(Segment& seg);
			U32 run();
		};

		const C8* name() const override { return "slp"; }
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;

		// diag
		static B32 envFlag(const C8* name);
		static B32 shapesDisabled();

		// impl
		static B32 packableElem(const Type* t);
		static B32 packableBinary(Opcode op, const Type* t);
		static B32 identifiedBase(const Node* n);
		static B32 isI64(const Node* n);
		static void refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		static void refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		static RefinedAddr refineAddr(Node* addr, U32 accessBytes);
		static String groupSig(const RefinedAddr& k);
		static B32 provablyDisjoint(const AliasAnalysis& aa,
																Node* pa,
																const RefinedAddr& ka,
																U32 sza,
																Node* pb,
																const RefinedAddr& kb,
																U32 szb);
		static B32 usesValue(const Node* u, const Node* x);
		static void rewriteInput(Node* u, const Node* from, Node* to);
		static StoreNode* soleChainSuccessor(StoreNode* s, List<LoadNode*>& observers);
		static U32 laneCountFor(U32 esz);
		static B32 windowAt(const Segment& seg, U32 at, WindowShape& out);
		static B32 windowHasObs(const Segment& seg, const WindowShape& ws);
		static B32 dataCone(const Node* root, U32 cap, List<const Node*>& out);
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
