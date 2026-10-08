// global code motion: recover a CFG and place each floating node into a basic
// block, hoisting out of loops where legal and otherwise sinking toward uses
//
// references:
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995
// - T. Lengauer and R. E. Tarjan, "A Fast Algorithm for Finding Dominators
//   in a Flowgraph", ACM TOPLAS, 1979

#ifndef RAT_CODEGEN_SCHEDULE_H
#define RAT_CODEGEN_SCHEDULE_H

#include "core.h"

#include "ir/node.h"

namespace rat {
	struct AliasAnalysis;
	struct Function;

	namespace detail {
		B32 storeMayAliasLoad(const AliasAnalysis& aa, const StoreNode* st, const LoadNode* ld);
		B32 earlierId(const Node* a, const Node* b);
	} // namespace detail

	struct Schedule {
		enum class TermKind {
			Return, // block ends in a return
			Branch, // block ends in a two-way If (thenB / elseB)
			Goto,		// block falls through to a single successor region (gotoB)
			Switch, // block ends in a multi-way branch (caseB, one per slot)
		};

		struct Block {
			Node* head = nullptr; // region, entry control proj, or if proj
			TermKind term = TermKind::Return;
			Node* termNode = nullptr;		// the return or if node (null for a goto)
			I32 thenB = -1, elseB = -1; // branch successors
			I32 gotoB = -1;							// goto successor (a region block)
			I32 gotoPredIdx = -1;				// which predecessor slot of gotoB this edge is
			List<I32> caseB;						// switch successors, slot order

			I32 idom = -1;				 // immediate dominator (entry dominates itself)
			I32 domDepth = 0;			 // depth in the dominator tree
			I32 loopDepth = 0;		 // number of natural loops containing this block
			I32 minDepthAbove = 0; // least loopDepth on the idom path up to entry
		};

		explicit Schedule(const Function& fn);
		// blockOf for the given loads only, dominators and loop depth
		Schedule(const Function& fn, const List<LoadNode*>& loads);

		I32 numBlocks() const;
		const Block& block(I32 b) const;
		const List<I32>& rpo() const;
		NodeSpan phis(I32 b) const;
		NodeSpan nodes(I32 b) const;
		const List<Node*>& allocNodes() const;

		I32 blockOf(const Node* n) const;

		U32 succCount(I32 b) const;
		I32 succAt(I32 b, U32 i) const;
		U32 predCount(I32 b) const;
		B32 dominates(I32 a, I32 b) const;

		static B32 isFloating(const Node* n);
		static B32 mayTrap(const Node* n);
	private:
		struct BlockEnd {
			Node* ifTerm = nullptr;
			Node* retTerm = nullptr;
			Node* gotoRegion = nullptr;
			I32 gotoIdx = -1;
		};

		I32 blockOfHead(const Node* head) const;
		I32 headBlock(const Node* head) const;

		void buildBlocks();
		void scanNodes();
		void buildCFG();
		void pin(Node* n, I32 b);
		BlockEnd walkBlock(I32 b);
		void setTerminator(I32 b, const BlockEnd& end);
		void computeRpo();
		void computeDominators();
		void computeLoops();
		void computeHoistBounds();
		void scheduleEarly(const List<Node*>& work, List<I32>& early);
		static Node* pendingInput(const Node* n, U32& next, const List<I32>& early);
		I32 deepestInput(const Node* n, const List<I32>& early) const;
		void scheduleLate(const List<Node*>& work, const List<I32>& early);
		static Node* pendingUser(const Node* n, U32& next, const List<U8>& done);
		I32 lateBlock(Node* n) const;
		void placeLoads(const List<Node*>& loads, const List<I32>& early);
		B32 place(Node* n, I32 late, const List<I32>& early);
		void buildBlockLists();
		static void bucket(const List<Node*>& items,
											 const List<I32>& keys,
											 U32 nb,
											 List<I32>& start,
											 List<Node*>& flat);

		static B32 isHeadNode(const Node* n);
		I32 ctrlBlock(Node* ctrl) const;

		I32 intersect(I32 a, I32 b) const;
		I32 lca(I32 a, I32 b) const;

		I32 useBlock(Node* u, Node* n) const;
		I32 predBlockForRegionInput(I32 regionBlock, U32 i) const;
		I32 hoistTarget(const Node* n, I32 late, I32 early) const;
		I32 homeBlock(Node* n) const;
		B32 isGuarded(const Node* n) const;

		struct TopoScratch {
			List<I32> localOf; // node id -> local index in the current block (-1)
			List<I32> inDeg;	 // per local index
			List<I32> stHead;	 // memory-state node id -> local index of the store/call consuming it (-1)
			List<I32> touchedSt;	// state node ids to reset after the block
			List<I32> succHead;		// per local index: head of the extra-edge chain (-1)
			List<I32> succNext;		// edge -> next edge in the chain
			List<I32> succTo;			// edge -> target local index
			List<I32> ready;			// binary heap of ready local indices, local order is id order
			List<Node*> out;			// topological order when some edge runs backward
			B32 backward = false; // some edge runs against local order

			I32 local(const Node* n) const;
			void addEdge(Node* before, Node* after);
			void push(I32 i);
			I32 pop();
		};
		void topoOrder(Node** nodes, U32 k, const AliasAnalysis& aa, TopoScratch& scratch) const;
		static void addAntiDeps(NodeSpan nodes, const AliasAnalysis& aa, TopoScratch& s);
		static void addOrderEdges(NodeSpan nodes, TopoScratch& s);

		I32 fixedDataBlock(Node* n, const List<I32>& early) const;
		static Node* requireProj(Node* n, U32 index);
		static Node* memoryInputOf(const Node* n);
		static B32 isMemWriter(const Node* n);
	private:
		const Function& fn;
		List<Block> blocks;
		mutable List<I32> headIndex; // node id -> block, for heads and memoized control nodes (-1)
		List<I32> nodeBlock;				 // node id -> block, for placed nodes (-1 = unplaced)
		List<I32> post;							 // postorder number per block
		List<I32> rpoOrder;
		I32 entryBlock = -1;
		List<C8> guarded;			// full schedule only, filled by scheduleEarly
		List<Node*> floating; // function order
		List<Node*> pinned;		// stores, calls, asm and stack ops, placed by their control
		List<Node*> dataPhis; // data phis
		List<Node*> allocs;		// stack objects, function order
		// block b owns [start[b], start[b + 1])
		List<I32> predStart;
		List<I32> predFlat;
		List<I32> phiStart;
		List<Node*> phiFlat;
		List<I32> nodeStart;
		List<Node*> nodeFlat;
	};
} // namespace rat

#endif
