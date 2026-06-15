#ifndef RAT_CODEGEN_SCHEDULE_H
#define RAT_CODEGEN_SCHEDULE_H

#include "Core.h"

#include <unordered_map>

namespace rat {
	struct Function;
	struct Node;
	struct PhiNode;

	struct Schedule {
		enum class TermKind {
			Return, // block ends in a return
			Branch, // block ends in a two-way If (thenB / elseB)
			Goto,		// block falls through to a single successor region (gotoB)
		};

		struct Block {
			Node* head = nullptr; // region, entry control proj, or if proj
			TermKind term = TermKind::Return;
			Node* termNode = nullptr; // the return or if node (null for a goto)
			I32 thenB = -1, elseB = -1; // branch successors
			I32 gotoB = -1; // goto successor (a region block)
			I32 gotoPredIdx = -1; // which predecessor slot of gotoB this edge is
			List<I32> preds; // predecessor block indices

			I32 idom = -1; // immediate dominator (entry dominates itself)
			I32 domDepth = 0; // depth in the dominator tree
			I32 loopDepth = 0; // number of natural loops containing this block

			List<PhiNode*> phis; // data phis merged at this block (region only)
			List<Node*> nodes; // scheduled compute nodes, in emit order
		};

		explicit Schedule(const Function& fn);

		const Function& function() const;
		I32 entry() const;
		I32 numBlocks() const;
		const Block& block(I32 b) const;
		const List<I32>& rpo() const;

		I32 blockOf(const Node* n) const;
		I32 blockOfHead(const Node* head) const;

		List<I32> successors(I32 b) const;
		B32 dominates(I32 a, I32 b) const;

		I32 idomOf(I32 b) const;
		I32 loopDepthOf(I32 b) const;

		static B32 isFloating(const Node* n);

	private:
		const Function& fn;
		List<Block> blocks;
		Map<const Node*, I32> headIndex; // head node -> block
		Map<const Node*, I32> nodeBlock; // placed node -> block
		List<I32> post; // postorder number per block
		List<I32> rpoOrder;
		I32 entryBlock = -1;

		void collectHeads();
		void buildCFG();
		void computeDominators();
		void computeLoops();
		void scheduleEarly(Map<const Node*, I32>& early);
		void scheduleLate(const Map<const Node*, I32>& early);
		void buildBlockLists();

		static B32 isHeadNode(const Node* n);
		Node* headOf(Node* ctrl) const;
		I32 intersectWith(const List<I32>& idom, I32 a, I32 b) const;
		I32 lca(I32 a, I32 b) const;
		I32 useBlock(Node* u, Node* n) const;
		I32 predBlockForRegionInput(I32 regionBlock, U32 i) const;
		List<Node*> topoOrder(List<Node*>& nodes) const;
	};
} // namespace rat

#endif
