// global code motion: recover a CFG and place each floating node into a basic
// block, hoisting out of loops where legal and otherwise sinking toward uses
//
// references:
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995
// - T. Lengauer and R. E. Tarjan, "A Fast Algorithm for Finding Dominators
//   in a Flowgraph", ACM TOPLAS, 1979

#include "CodeGen/Schedule.h"

#include "IR/Function.h"
#include "IR/Node.h"

#include <cassert>
#include <functional>
#include <queue>
#include <unordered_set>

namespace rat {

	Schedule::Schedule(const Function& fn) : fn(fn) {
		collectHeads();
		buildCFG();
		computeDominators();
		computeLoops();
		Map<const Node*, I32> early;
		scheduleEarly(early);
		scheduleLate(early);
		buildBlockLists();
	}

	const Function& Schedule::function() const { return fn; }
	I32 Schedule::entry() const { return entryBlock; }
	I32 Schedule::numBlocks() const { return (I32)blocks.size(); }
	const Schedule::Block& Schedule::block(I32 b) const { return blocks[b]; }
	const List<I32>& Schedule::rpo() const { return rpoOrder; }
	I32 Schedule::idomOf(I32 b) const { return blocks[b].idom; }
	I32 Schedule::loopDepthOf(I32 b) const { return blocks[b].loopDepth; }

	B32 Schedule::isHeadNode(const Node* n) {
		if (n->getOpcode() == Opcode::Region)
			return true;
		if (ProjNode* p = dyn_cast<ProjNode>(const_cast<Node*>(n))) {
			Node* prod = p->getProducer();
			if (prod->getOpcode() == Opcode::If)
				return true;
			if (prod->getOpcode() == Opcode::Start && p->getIndex() == 0)
				return true;
		}
		return false;
	}

	void Schedule::collectHeads() {
		for (Node* n : fn) {
			if (isHeadNode(n)) {
				headIndex[n] = (I32)blocks.size();
				blocks.emplace_back();
				blocks.back().head = n;
				if (ProjNode* p = dyn_cast<ProjNode>(n))
					if (p->getProducer()->getOpcode() == Opcode::Start)
						entryBlock = headIndex[n];
			}
		}
		assert(entryBlock >= 0 && "no entry block found");
	}

	I32 Schedule::blockOfHead(const Node* head) const {
		auto it = headIndex.find(head);
		return it == headIndex.end() ? -1 : it->second;
	}

	Node* Schedule::headOf(Node* ctrl) const {
		while (true) {
			if (isHeadNode(ctrl))
				return ctrl;
			ProjNode* p = cast<ProjNode>(ctrl);
			CallNode* c = cast<CallNode>(p->getProducer());
			ctrl = c->getControlInput();
		}
	}

	namespace detail {
		Node* controlProjOf(Node* call) {
			Node* p = call->projection(CallNode::controlProjIndex());
			assert(p && "call without a control projection");
			return p;
		}

		Node* projOfIf(Node* iff, U32 index) {
			Node* p = iff->projection(index);
			assert(p && "if without expected projection");
			return p;
		}
	} // namespace detail
	using namespace detail;

	void Schedule::buildCFG() {
		for (I32 b = 0; b < (I32)blocks.size(); ++b) {
			Node* cur = blocks[b].head;
			while (true) {
				Node* nextCall = nullptr;
				Node* ifTerm = nullptr;
				Node* retTerm = nullptr;
				Node* gotoRegion = nullptr;
				I32 gotoIdx = -1;

				for (Node* u : cur->getUsers()) {
					switch (u->getOpcode()) {
					case Opcode::Store:
						if (u->getControlInput() == cur)
							nodeBlock[u] = b;
						break;
					case Opcode::Call:
						if (u->getControlInput() == cur) {
							nodeBlock[u] = b;
							nextCall = u;
						}
						break;
					case Opcode::If:
						if (u->getControlInput() == cur)
							ifTerm = u;
						break;
					case Opcode::Return:
						if (u->getControlInput() == cur)
							retTerm = u;
						break;
					case Opcode::Region:
						for (U32 k = 0, e = u->getInputCount(); k < e; ++k)
							if (u->getInput(k) == cur) {
								gotoRegion = u;
								gotoIdx = (I32)k;
							}
						break;
					default:
						break;
					}
				}

				if (nextCall) {
					cur = controlProjOf(nextCall);
					continue;
				}

				Block& t = blocks[b];
				if (ifTerm) {
					t.term = TermKind::Branch;
					t.termNode = ifTerm;
					Node* thenP = projOfIf(ifTerm, IfNode::thenProjIndex());
					Node* elseP = projOfIf(ifTerm, IfNode::elseProjIndex());
					t.thenB = headIndex.at(thenP);
					t.elseB = headIndex.at(elseP);
					blocks[t.thenB].preds.push_back(b);
					blocks[t.elseB].preds.push_back(b);
				} else if (retTerm) {
					t.term = TermKind::Return;
					t.termNode = retTerm;
				} else {
					assert(gotoRegion && "block has no terminator");
					t.term = TermKind::Goto;
					t.gotoB = headIndex.at(gotoRegion);
					t.gotoPredIdx = gotoIdx;
					blocks[t.gotoB].preds.push_back(b);
				}
				break;
			}
		}
	}

	List<I32> Schedule::successors(I32 b) const {
		const Block& t = blocks[b];
		switch (t.term) {
		case TermKind::Branch:
			return {t.thenB, t.elseB};
		case TermKind::Goto:
			return {t.gotoB};
		case TermKind::Return:
			return {};
		}
		return {};
	}

	void Schedule::computeDominators() {
		I32 count = (I32)blocks.size();
		post.assign(count, -1);
		List<I32> order;
		List<char> visited(count, 0);

		std::function<void(I32)> dfs = [&](I32 b) {
			visited[b] = 1;
			for (I32 s : successors(b))
				if (!visited[s])
					dfs(s);
			post[b] = (I32)order.size();
			order.push_back(b);
		};
		dfs(entryBlock);
		rpoOrder.assign(order.rbegin(), order.rend());

		List<I32> idom(count, -1);
		idom[entryBlock] = entryBlock;
		B32 changed = true;
		while (changed) {
			changed = false;
			for (I32 b : rpoOrder) {
				if (b == entryBlock)
					continue;
				I32 newIdom = -1;
				for (I32 p : blocks[b].preds) {
					if (idom[p] == -1)
						continue;
					newIdom = (newIdom == -1) ? p : intersectWith(idom, p, newIdom);
				}
				if (newIdom != -1 && idom[b] != newIdom) {
					idom[b] = newIdom;
					changed = true;
				}
			}
		}
		for (I32 b = 0; b < count; ++b)
			blocks[b].idom = idom[b];

		for (I32 b : rpoOrder)
			blocks[b].domDepth =
					(b == entryBlock) ? 0 : blocks[blocks[b].idom].domDepth + 1;
	}

	I32 Schedule::intersectWith(const List<I32>& idom, I32 a, I32 b) const {
		while (a != b) {
			while (post[a] < post[b])
				a = idom[a];
			while (post[b] < post[a])
				b = idom[b];
		}
		return a;
	}

	B32 Schedule::dominates(I32 a, I32 b) const {
		while (blocks[b].domDepth > blocks[a].domDepth)
			b = blocks[b].idom;
		return a == b;
	}

	I32 Schedule::lca(I32 a, I32 b) const {
		if (a < 0)
			return b;
		if (b < 0)
			return a;
		while (blocks[a].domDepth > blocks[b].domDepth)
			a = blocks[a].idom;
		while (blocks[b].domDepth > blocks[a].domDepth)
			b = blocks[b].idom;
		while (a != b) {
			a = blocks[a].idom;
			b = blocks[b].idom;
		}
		return a;
	}

	void Schedule::computeLoops() {
		Map<I32, Set<I32>> loops;
		for (I32 b = 0; b < (I32)blocks.size(); ++b)
			for (I32 s : successors(b))
				if (dominates(s, b)) { // b -> s is a back-edge; s is a loop header
					Set<I32>& body = loops[s];
					body.insert(s); // header bounds the backward walk
					List<I32> stack;
					if (body.insert(b).second)
						stack.push_back(b);
					while (!stack.empty()) {
						I32 x = stack.back();
						stack.pop_back();
						for (I32 p : blocks[x].preds)
							if (body.insert(p).second) // stops at the header
								stack.push_back(p);
					}
				}

		for (auto& kv : loops)
			for (I32 m : kv.second)
				++blocks[m].loopDepth;
	}

	B32 Schedule::isFloating(const Node* n) {
		Opcode op = n->getOpcode();
		return op == Opcode::Load || isBinaryOpcode(op) || isUnaryOpcode(op) ||
					 isCompareOpcode(op) || isConvertOpcode(op);
	}

	namespace detail {
		I32 fixedDataBlock(const Schedule& s, Node* n,
											 const Map<const Node*, I32>& early) {
			if (Schedule::isFloating(n)) {
				auto it = early.find(n);
				return it == early.end() ? -1 : it->second;
			}
			switch (n->getOpcode()) {
			case Opcode::Phi:
				return s.blockOfHead(cast<PhiNode>(n)->getRegion());
			case Opcode::Load:
			case Opcode::Store:
			case Opcode::Call:
				return s.blockOf(n);
			case Opcode::Proj: {
				ProjNode* p = cast<ProjNode>(n);
				Node* prod = p->getProducer();
				if (prod->getOpcode() == Opcode::Call)
					return s.blockOf(prod); // call value/control projection
				return -1;
			}
			case Opcode::Constant:
			default:
				return -1; // no constraint
			}
		}
	} // namespace detail

	void Schedule::scheduleEarly(Map<const Node*, I32>& early) {
		List<Node*> work;
		for (Node* n : fn)
			if (isFloating(n))
				work.push_back(n);

		for (Node* n : work)
			early[n] = entryBlock;

		// deepest input block, to fixpoint (a floating input may not be settled)
		B32 changed = true;
		while (changed) {
			changed = false;
			for (Node* n : work) {
				I32 e = entryBlock;
				U32 first = (n->getOpcode() == Opcode::Load) ? 1 : 0;
				for (U32 i = first, ie = n->getInputCount(); i < ie; ++i) {
					Node* in = n->getInput(i);
					if (!in)
						continue;
					I32 b = fixedDataBlock(*this, in, early);
					if (b >= 0 && blocks[b].domDepth > blocks[e].domDepth)
						e = b;
				}
				if (early[n] != e) {
					early[n] = e;
					changed = true;
				}
			}
		}
	}

	I32 Schedule::useBlock(Node* u, Node* n) const {
		if (PhiNode* phi = dyn_cast<PhiNode>(u)) {
			I32 rb = headIndex.at(phi->getRegion());
			I32 acc = -1;
			for (U32 i = 0, e = phi->getValueCount(); i < e; ++i)
				if (phi->getValue(i) == n)
					acc = lca(acc, predBlockForRegionInput(rb, i));
			return acc < 0 ? rb : acc;
		}
		if (isFloating(u)) {
			auto it = nodeBlock.find(u);
			return it == nodeBlock.end() ? -1 : it->second;
		}
		// pinned user (store/load/call/return/if): its own block
		auto it = nodeBlock.find(u);
		if (it != nodeBlock.end())
			return it->second;
		if (u->getOpcode() == Opcode::Return || u->getOpcode() == Opcode::If)
			return headIndex.at(headOf(u->getControlInput()));
		return -1;
	}

	I32 Schedule::predBlockForRegionInput(I32 rb, U32 i) const {
		Node* region = blocks[rb].head;
		Node* predCtrl = region->getInput(i);
		return headIndex.at(headOf(predCtrl));
	}

	void Schedule::scheduleLate(const Map<const Node*, I32>& early) {
		List<Node*> work;
		for (Node* n : fn)
			if (isFloating(n))
				work.push_back(n);

		// late = LCA of use blocks; then hoist to the shallowest loop depth on the
		// dominator path between early and late. Iterated to a fixpoint
		B32 changed = true;
		while (changed) {
			changed = false;
			for (Node* n : work) {
				I32 late;
				if (n->getOpcode() == Opcode::Load) {
					// floating loads move up from where they were built but never
					// below it: the home block is a sound late bound
					late = headIndex.at(headOf(n->getControlInput()));
				} else {
					late = -1;
					for (Node* u : n->getUsers())
						late = lca(late, useBlock(u, n));
					if (late < 0)
						continue; // no placed use yet
				}

				I32 e = early.count(n) ? early.at(n) : entryBlock;
				// walk from late up toward early, remembering the block with the
				// smallest loop depth (ties keep the deepest = latest, found first)
				I32 cur = late, pick = late;
				while (true) {
					if (blocks[cur].loopDepth < blocks[pick].loopDepth)
						pick = cur;
					if (cur == e || cur == entryBlock)
						break;
					I32 next = blocks[cur].idom;
					if (next == cur)
						break;
					cur = next;
				}

				auto it = nodeBlock.find(n);
				if (it == nodeBlock.end() || it->second != pick) {
					nodeBlock[n] = pick;
					changed = true;
				}
			}
		}
	}

	I32 Schedule::blockOf(const Node* n) const {
		auto it = nodeBlock.find(n);
		return it == nodeBlock.end() ? -1 : it->second;
	}

	void Schedule::buildBlockLists() {
		for (Node* n : fn)
			if (PhiNode* phi = dyn_cast<PhiNode>(n))
				if (phi->getType()->isData())
					blocks[headIndex.at(phi->getRegion())].phis.push_back(phi);

		List<List<Node*>> raw(blocks.size());
		for (Node* n : fn) {
			Opcode op = n->getOpcode();
			B32 pinned = (op == Opcode::Store || op == Opcode::Call);
			if (pinned || isFloating(n)) {
				auto it = nodeBlock.find(n);
				if (it != nodeBlock.end())
					raw[it->second].push_back(n);
			}
		}
		for (I32 b = 0; b < (I32)blocks.size(); ++b)
			blocks[b].nodes = topoOrder(raw[b]);
	}

	List<Node*> Schedule::topoOrder(List<Node*>& nodes) const {
		Set<const Node*> inBlock(nodes.begin(), nodes.end());
		Map<const Node*, I32> inDeg;
		inDeg.reserve(nodes.size() * 2);

		auto laterId = [](const Node* a, const Node* b) {
			return a->getId() > b->getId();
		};
		std::priority_queue<Node*, std::vector<Node*>, decltype(laterId)> ready(
				laterId);

		for (Node* n : nodes) {
			I32 d = 0;
			for (U32 i = 0, e = n->getInputCount(); i < e; ++i) {
				Node* in = n->getInput(i);
				if (in && inBlock.count(in))
					++d;
			}
			inDeg[n] = d;
			if (d == 0)
				ready.push(n);
		}

		List<Node*> out;
		out.reserve(nodes.size());
		while (!ready.empty()) {
			Node* n = ready.top();
			ready.pop();
			out.push_back(n);
			for (Node* u : n->getUsers())
				if (inBlock.count(u) && --inDeg[u] == 0)
					ready.push(u);
		}
		assert(out.size() == nodes.size() && "cycle in intra-block schedule");
		return out;
	}
} // namespace rat
