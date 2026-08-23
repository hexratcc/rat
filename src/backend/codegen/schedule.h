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

namespace rat {
	struct AliasAnalysis;
	struct Function;
	struct Node;
	struct PhiNode;

	namespace detail {
		I32 idGet(const List<I32>& v, U32 id);
		void idSet(List<I32>& v, U32 id, I32 val);
		Node* nodeGet(const List<Node*>& v, U32 id);
		void nodeSet(List<Node*>& v, U32 id, Node* val);
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
			List<I32> preds;						// predecessor block indices

			I32 idom = -1;		 // immediate dominator (entry dominates itself)
			I32 domDepth = 0;	 // depth in the dominator tree
			I32 loopDepth = 0; // number of natural loops containing this block

			List<PhiNode*> phis; // data phis merged at this block (region only)
			List<Node*> nodes;	 // scheduled compute nodes, in emit order
		};

		explicit Schedule(const Function& fn);

		I32 numBlocks() const;
		const Block& block(I32 b) const;
		const List<I32>& rpo() const;

		I32 blockOf(const Node* n) const;

		List<I32> successors(I32 b) const;
		void successorsInto(I32 b, List<I32>& out) const;
		B32 dominates(I32 a, I32 b) const;

		static B32 isFloating(const Node* n);
	private:
		I32 blockOfHead(const Node* head) const;
		I32 headBlock(const Node* head) const;
		U32 succCount(I32 b) const;
		I32 succAt(I32 b, U32 i) const;

		void collectHeads();
		void buildCFG();
		void computeDominators();
		void computeLoops();
		void scheduleEarly(List<I32>& early);
		void scheduleLate(const List<I32>& early);
		void buildBlockLists();

		static B32 isHeadNode(const Node* n);
		Node* headOf(Node* ctrl) const;

		I32 intersectWith(const List<I32>& idom, I32 a, I32 b) const;
		I32 lca(I32 a, I32 b) const;

		I32 useBlock(Node* u, Node* n) const;
		I32 predBlockForRegionInput(I32 regionBlock, U32 i) const;

		struct TopoScratch {
			List<I32> localOf;		// node id -> local index in the current block (-1)
			List<I32> inDeg;			// per local index
			List<I32> memHead;		// memory node id -> local index of a load on it (-1)
			List<I32> memNext;		// per local index: next load sharing that memory
			List<I32> touchedMem; // memory node ids to reset after the block
			List<I32> stHead;     // memory-state node id -> local index of the store/call consuming it (-1)
			List<I32> touchedSt;  // state node ids to reset after the block
			List<I32> succHead;	  // per local index: head of the extra-edge chain (-1)
			List<I32> succNext;	  // edge -> next edge in the chain
			List<I32> succTo;		  // edge -> target local index
			List<Node*> ready;	  // binary heap of ready nodes
		};
		List<Node*> topoOrder(List<Node*>& nodes, const AliasAnalysis& aa, TopoScratch& scratch) const;

		I32 fixedDataBlock(Node* n, const List<I32>& early) const;
		static Node* requireProj(Node* n, U32 index);
		static Node* memoryInputOf(const Node* n);
	private:
		const Function& fn;
		List<Block> blocks;
		List<I32> headIndex; // node id -> block, for head nodes (-1 = not a head)
		List<I32> nodeBlock; // node id -> block, for placed nodes (-1 = unplaced)
		List<I32> post;			 // postorder number per block
		List<I32> rpoOrder;
		I32 entryBlock = -1;
		mutable List<Node*> headMemo;
	};
} // namespace rat

#endif
