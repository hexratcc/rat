#include "codegen/schedule.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"

namespace rat {
	namespace detail {
		I32 idGet(const List<I32>& v, U32 id) { return id < v.size() ? v[id] : -1; }

		void idSet(List<I32>& v, U32 id, I32 val) {
			if(id >= v.size())
				v.resize(id + 1, -1);
			v[id] = val;
		}

		Node* nodeGet(const List<Node*>& v, U32 id) { return id < v.size() ? v[id] : nullptr; }

		void nodeSet(List<Node*>& v, U32 id, Node* val) {
			if(id >= v.size())
				v.resize(id + 1, nullptr);
			v[id] = val;
		}
	} // namespace detail

	Schedule::Schedule(const Function& fn)
	: fn(fn) {
		U32 count = fn.size();
		headIndex.assign(count, -1);
		nodeBlock.assign(count, -1);
		headMemo.assign(count, nullptr);
		collectHeads();
		buildCFG();
		computeDominators();
		computeLoops();
		List<I32> early(count, -1);
		scheduleEarly(early);
		scheduleLate(early);
		buildBlockLists();
	}

	I32 Schedule::numBlocks() const { return (I32)blocks.size(); }
	const Schedule::Block& Schedule::block(I32 b) const { return blocks[b]; }
	const List<I32>& Schedule::rpo() const { return rpoOrder; }

	Node* Schedule::requireProj(Node* n, U32 index) {
		Node* p = n->projection(index);
		assert(p && "expected projection is missing");
		return p;
	}

	B32 Schedule::isHeadNode(const Node* n) {
		if(isa<RegionNode>(n))
			return true;
		if(const ProjNode* p = dyn_cast<ProjNode>(n)) {
			Node* prod = p->getProducer();
			if(isa<IfNode>(prod) || isa<SwitchNode>(prod))
				return true;
			if(isa<StartNode>(prod) && p->getIndex() == 0)
				return true;
		}
		return false;
	}

	void Schedule::collectHeads() {
		for(Node* n : fn) {
			if(isHeadNode(n)) {
				I32 b = (I32)blocks.size();
				detail::idSet(headIndex, n->getId(), b);
				blocks.emplace_back();
				blocks.back().head = n;
				if(ProjNode* p = dyn_cast<ProjNode>(n))
					if(isa<StartNode>(p->getProducer()))
						entryBlock = b;
			}
		}
		assert(entryBlock >= 0 && "no entry block found");
	}

	I32 Schedule::blockOfHead(const Node* head) const {
		return head ? detail::idGet(headIndex, head->getId()) : -1;
	}

	I32 Schedule::headBlock(const Node* head) const {
		I32 b = blockOfHead(head);
		assert(b >= 0 && "control node has no block");
		return b;
	}

	Node* Schedule::headOf(Node* ctrl) const {
		List<Node*> path;
		Node* c = ctrl;
		while(true) {
			if(Node* memo = detail::nodeGet(headMemo, c->getId())) {
				c = memo;
				break;
			}
			if(isHeadNode(c))
				break;
			path.push_back(c);
			ProjNode* p = cast<ProjNode>(c);
			CallNode* call = cast<CallNode>(p->getProducer());
			c = call->getControlInput();
		}
		for(Node* n : path)
			detail::nodeSet(headMemo, n->getId(), c);
		return c;
	}

	void Schedule::buildCFG() {
		for(I32 b = 0; b < (I32)blocks.size(); ++b) {
			Node* cur = blocks[b].head;
			while(true) {
				Node* nextCall = nullptr;
				Node* ifTerm = nullptr;
				Node* retTerm = nullptr;
				Node* gotoRegion = nullptr;
				I32 gotoIdx = -1;

				for(Node* u : cur->getUsers()) {
					switch(u->getOpcode()) {
					case Opcode::Store:
						if(u->getControlInput() == cur)
							detail::idSet(nodeBlock, u->getId(), b);
						break;
					case Opcode::Call:
						if(u->getControlInput() == cur) {
							detail::idSet(nodeBlock, u->getId(), b);
							nextCall = u;
						}
						break;
					case Opcode::If:
					case Opcode::Switch:
						if(u->getControlInput() == cur)
							ifTerm = u;
						break;
					case Opcode::Return:
						if(u->getControlInput() == cur)
							retTerm = u;
						break;
					case Opcode::Region:
						for(U32 k = 0, e = u->getInputCount(); k < e; ++k)
							if(u->getInput(k) == cur) {
								gotoRegion = u;
								gotoIdx = (I32)k;
							}
						break;
					default:
						break;
					}
				}

				if(nextCall) {
					cur = requireProj(nextCall, CallNode::controlProjIndex());
					continue;
				}

				Block& t = blocks[b];
				if(ifTerm && ifTerm->getOpcode() == Opcode::Switch) {
					t.term = TermKind::Switch;
					t.termNode = ifTerm;
					SwitchNode* sw = cast<SwitchNode>(ifTerm);
					for(U32 k = 0, e = sw->getSlotCount(); k < e; ++k) {
						I32 tb = headBlock(requireProj(ifTerm, k));
						t.caseB.push_back(tb);
						blocks[tb].preds.push_back(b);
					}
				} else if(ifTerm) {
					t.term = TermKind::Branch;
					t.termNode = ifTerm;
					Node* thenP = requireProj(ifTerm, IfNode::thenProjIndex());
					Node* elseP = requireProj(ifTerm, IfNode::elseProjIndex());
					t.thenB = headBlock(thenP);
					t.elseB = headBlock(elseP);
					blocks[t.thenB].preds.push_back(b);
					blocks[t.elseB].preds.push_back(b);
				} else if(retTerm) {
					t.term = TermKind::Return;
					t.termNode = retTerm;
				} else {
					assert(gotoRegion && "block has no terminator");
					t.term = TermKind::Goto;
					t.gotoB = headBlock(gotoRegion);
					t.gotoPredIdx = gotoIdx;
					blocks[t.gotoB].preds.push_back(b);
				}
				break;
			}
		}
	}

	U32 Schedule::succCount(I32 b) const {
		const Block& t = blocks[b];
		switch(t.term) {
		case TermKind::Branch:
			return 2;
		case TermKind::Goto:
			return 1;
		case TermKind::Switch:
			return (U32)t.caseB.size();
		case TermKind::Return:
			return 0;
		}
		return 0;
	}

	I32 Schedule::succAt(I32 b, U32 i) const {
		const Block& t = blocks[b];
		switch(t.term) {
		case TermKind::Branch:
			return i == 0 ? t.thenB : t.elseB;
		case TermKind::Goto:
			return t.gotoB;
		case TermKind::Switch:
			return t.caseB[i];
		case TermKind::Return:
			break;
		}
		return -1;
	}

	void Schedule::successorsInto(I32 b, List<I32>& out) const {
		out.clear();
		for(U32 i = 0, e = succCount(b); i < e; ++i)
			out.push_back(succAt(b, i));
	}

	List<I32> Schedule::successors(I32 b) const {
		List<I32> out;
		successorsInto(b, out);
		return out;
	}

	void Schedule::computeDominators() {
		I32 count = (I32)blocks.size();
		post.assign(count, -1);
		List<I32> order;
		List<C8> visited(count, 0);

		struct Frame {
			I32 block;
			U32 next = 0;
		};

		List<Frame> stack;
		stack.reserve(count);
		visited[entryBlock] = 1;
		stack.push_back({entryBlock, 0});
		while(!stack.empty()) {
			Frame& f = stack.back();
			if(f.next < succCount(f.block)) {
				I32 s = succAt(f.block, f.next++);
				if(!visited[s]) {
					visited[s] = 1;
					stack.push_back({s, 0});
				}
			} else {
				post[f.block] = (I32)order.size();
				order.push_back(f.block);
				stack.pop_back();
			}
		}
		rpoOrder.assign(order.rbegin(), order.rend());

		List<I32> idom(count, -1);
		idom[entryBlock] = entryBlock;
		B32 changed = true;
		while(changed) {
			changed = false;
			for(I32 b : rpoOrder) {
				if(b == entryBlock)
					continue;
				I32 newIdom = -1;
				for(I32 p : blocks[b].preds) {
					if(idom[p] == -1)
						continue;
					newIdom = (newIdom == -1) ? p : intersectWith(idom, p, newIdom);
				}
				if(newIdom != -1 && idom[b] != newIdom) {
					idom[b] = newIdom;
					changed = true;
				}
			}
		}
		for(I32 b = 0; b < count; ++b)
			blocks[b].idom = idom[b];

		for(I32 b : rpoOrder)
			blocks[b].domDepth = (b == entryBlock) ? 0 : blocks[blocks[b].idom].domDepth + 1;
	}

	I32 Schedule::intersectWith(const List<I32>& idom, I32 a, I32 b) const {
		while(a != b) {
			while(post[a] < post[b])
				a = idom[a];
			while(post[b] < post[a])
				b = idom[b];
		}
		return a;
	}

	B32 Schedule::dominates(I32 a, I32 b) const {
		while(blocks[b].domDepth > blocks[a].domDepth)
			b = blocks[b].idom;
		return a == b;
	}

	I32 Schedule::lca(I32 a, I32 b) const {
		if(a < 0)
			return b;
		if(b < 0)
			return a;
		while(blocks[a].domDepth > blocks[b].domDepth)
			a = blocks[a].idom;
		while(blocks[b].domDepth > blocks[a].domDepth)
			b = blocks[b].idom;
		while(a != b) {
			a = blocks[a].idom;
			b = blocks[b].idom;
		}
		return a;
	}

	void Schedule::computeLoops() {
		I32 count = (I32)blocks.size();
		List<I32> backHead(count, -1); // header -> first back-edge
		List<I32> backNext;						 // back-edge -> next edge of the same header
		List<I32> backFrom;						 // back-edge -> tail block
		for(I32 b = 0; b < count; ++b)
			for(U32 i = 0, e = succCount(b); i < e; ++i) {
				I32 s = succAt(b, i);
				if(!dominates(s, b)) // b -> s is a back-edge; s is a loop header
					continue;
				backFrom.push_back(b);
				backNext.push_back(backHead[s]);
				backHead[s] = (I32)backFrom.size() - 1;
			}

		List<U32> stamp(count, 0);
		U32 gen = 0;
		List<I32> stack;
		for(I32 h = 0; h < count; ++h) {
			if(backHead[h] < 0)
				continue;
			U32 g = ++gen;
			stamp[h] = g; // header bounds the backward walk
			++blocks[h].loopDepth;
			for(I32 e = backHead[h]; e >= 0; e = backNext[e]) {
				I32 b = backFrom[e];
				if(stamp[b] != g) {
					stamp[b] = g;
					++blocks[b].loopDepth;
					stack.push_back(b);
				}
				while(!stack.empty()) {
					I32 x = stack.back();
					stack.pop_back();
					for(I32 p : blocks[x].preds)
						if(stamp[p] != g) { // stops at the header
							stamp[p] = g;
							++blocks[p].loopDepth;
							stack.push_back(p);
						}
				}
			}
		}
	}

	B32 Schedule::isFloating(const Node* n) {
		Opcode op = n->getOpcode();
		if(op == Opcode::Alloc)
			return cast<AllocNode>(n)->isVariableSized();
		if(op == Opcode::Global)
			return true;
		if(op == Opcode::Constant)
			return n->getType()->isFloat() && n->getType()->getFloatWidth() != 128;
		return op == Opcode::Load || isArithmeticOpcode(op) || isVectorUtilOpcode(op);
	}

	I32 Schedule::fixedDataBlock(Node* n, const List<I32>& early) const {
		if(isFloating(n))
			return detail::idGet(early, n->getId());
		switch(n->getOpcode()) {
		case Opcode::Phi:
			return blockOfHead(cast<PhiNode>(n)->getRegion());
		case Opcode::Store:
		case Opcode::Call:
			return blockOf(n);
		case Opcode::Proj: {
			Node* prod = cast<ProjNode>(n)->getProducer();
			if(isa<CallNode>(prod))
				return blockOf(prod); // call value/control projection
			return -1;
		}
		case Opcode::Constant:
		default:
			return -1; // no constraint
		}
	}

	void Schedule::scheduleEarly(List<I32>& early) {
		List<Node*> work;
		for(Node* n : fn)
			if(isFloating(n))
				work.push_back(n);

		for(Node* n : work)
			detail::idSet(early, n->getId(), entryBlock);

		// deepest input block, to fixpoint (a floating input may not be settled)
		B32 changed = true;
		while(changed) {
			changed = false;
			for(Node* n : work) {
				I32 e = entryBlock;
				U32 first = isa<LoadNode>(n) ? 1 : 0;
				for(U32 i = first, ie = n->getInputCount(); i < ie; ++i) {
					Node* in = n->getInput(i);
					if(!in)
						continue;
					I32 b = fixedDataBlock(in, early);
					if(b >= 0 && blocks[b].domDepth > blocks[e].domDepth)
						e = b;
				}
				U32 id = n->getId();
				if(detail::idGet(early, id) != e) {
					detail::idSet(early, id, e);
					changed = true;
				}
			}
		}
	}

	I32 Schedule::useBlock(Node* u, Node* n) const {
		if(PhiNode* phi = dyn_cast<PhiNode>(u)) {
			I32 rb = headBlock(phi->getRegion());
			I32 acc = -1;
			for(U32 i = 0, e = phi->getValueCount(); i < e; ++i)
				if(phi->getValue(i) == n)
					acc = lca(acc, predBlockForRegionInput(rb, i));
			return acc < 0 ? rb : acc;
		}
		I32 b = detail::idGet(nodeBlock, u->getId());
		if(b >= 0)
			return b;
		if(isa<ReturnNode>(u) || isa<IfNode>(u))
			return headBlock(headOf(u->getControlInput()));
		return -1;
	}

	I32 Schedule::predBlockForRegionInput(I32 rb, U32 i) const {
		Node* region = blocks[rb].head;
		Node* predCtrl = region->getInput(i);
		return headBlock(headOf(predCtrl));
	}

	void Schedule::scheduleLate(const List<I32>& early) {
		List<Node*> work;
		for(Node* n : fn)
			if(isFloating(n))
				work.push_back(n);

		// late = LCA of use blocks; then hoist to the shallowest loop depth on the
		// dominator path between early and late. Iterated to a fixpoint
		B32 changed = true;
		while(changed) {
			changed = false;
			for(U32 wi = (U32)work.size(); wi > 0; --wi) {
				Node* n = work[wi - 1];
				I32 late;
				if(isa<LoadNode>(n)) {
					// floating loads move up from where they were built but never
					// below it: the home block is a sound late bound
					late = headBlock(headOf(n->getControlInput()));
				} else {
					late = -1;
					for(Node* u : n->getUsers())
						late = lca(late, useBlock(u, n));
					if(late < 0)
						continue; // no placed use yet
				}

				I32 e = detail::idGet(early, n->getId());
				if(e < 0)
					e = entryBlock;
				Opcode op = n->getOpcode();
				B32 remat = op == Opcode::Constant || op == Opcode::Global;
				// walk from late up toward early, remembering the block with the
				// smallest loop depth (ties keep the deepest = latest, found first)
				I32 cur = late, pick = late;
				while(true) {
					if(blocks[cur].loopDepth < blocks[pick].loopDepth) {
						pick = cur;
						if(remat)
							break; // first depth drop only
					}
					if(cur == e || cur == entryBlock)
						break;
					I32 next = blocks[cur].idom;
					if(next == cur)
						break;
					cur = next;
				}

				U32 id = n->getId();
				if(detail::idGet(nodeBlock, id) != pick) {
					detail::idSet(nodeBlock, id, pick);
					changed = true;
				}
			}
		}
	}

	I32 Schedule::blockOf(const Node* n) const {
		return n ? detail::idGet(nodeBlock, n->getId()) : -1;
	}

	void Schedule::buildBlockLists() {
		for(Node* n : fn)
			if(PhiNode* phi = dyn_cast<PhiNode>(n))
				if(phi->getType()->isData())
					blocks[headBlock(phi->getRegion())].phis.push_back(phi);

		List<List<Node*>> raw(blocks.size());
		for(Node* n : fn) {
			B32 pinned = isa<StoreNode>(n) || isa<CallNode>(n);
			if(pinned || isFloating(n)) {
				I32 b = detail::idGet(nodeBlock, n->getId());
				if(b >= 0)
					raw[b].push_back(n);
			}
		}
		TopoScratch scratch;
		scratch.localOf.assign(fn.size(), -1);
		scratch.memHead.assign(fn.size(), -1);
		AliasAnalysis aa(fn, 8);
		for(I32 b = 0; b < (I32)blocks.size(); ++b)
			blocks[b].nodes = topoOrder(raw[b], aa, scratch);
	}

	Node* Schedule::memoryInputOf(const Node* n) {
		if(const LoadNode* l = dyn_cast<LoadNode>(n))
			return l->getMemory();
		if(const StoreNode* s = dyn_cast<StoreNode>(n))
			return s->getMemory();
		if(const CallNode* c = dyn_cast<CallNode>(n))
			return c->getMemory();
		return nullptr;
	}

	namespace {
		B32 storeMayAliasLoad(const AliasAnalysis& aa, const StoreNode* st, const LoadNode* ld) {
			return aa.alias(
								 st->getPointer(), aa.getAccessSize(st), ld->getPointer(), aa.getAccessSize(ld)) !=
						 AliasResult::NoAlias;
		}
	} // namespace

	List<Node*>
	Schedule::topoOrder(List<Node*>& nodes, const AliasAnalysis& aa, TopoScratch& s) const {
		U32 k = (U32)nodes.size();
		List<Node*> out;
		out.reserve(k);
		if(k == 0)
			return out;

		for(U32 i = 0; i < k; ++i)
			detail::idSet(s.localOf, nodes[i]->getId(), (I32)i);

		auto local = [&](const Node* n) -> I32 {
			return n ? detail::idGet(s.localOf, n->getId()) : -1;
		};

		s.inDeg.assign(k, 0);
		s.succHead.assign(k, -1);
		s.succNext.clear();
		s.succTo.clear();
		s.memNext.assign(k, -1);

		for(U32 i = 0; i < k; ++i) {
			Node* n = nodes[i];
			if(!isa<LoadNode>(n))
				continue;
			Node* m = memoryInputOf(n);
			if(!m)
				continue;
			U32 mid = m->getId();
			I32 head = detail::idGet(s.memHead, mid);
			if(head < 0)
				s.touchedMem.push_back((I32)mid);
			s.memNext[i] = head;
			detail::idSet(s.memHead, mid, (I32)i);
		}

		// extra ordering edges
		auto addEdge = [&](Node* before, Node* after) {
			if(before == after)
				return;
			I32 bi = local(before), ai = local(after);
			if(bi < 0 || ai < 0)
				return;
			s.succTo.push_back(ai);
			s.succNext.push_back(s.succHead[bi]);
			s.succHead[bi] = (I32)s.succTo.size() - 1;
			++s.inDeg[ai];
		};

		// WAR anti-deps. a store/call totally orders the block's memory
		for(U32 i = 0; i < k; ++i) {
			Node* n = nodes[i];
			if(!isa<StoreNode>(n) && !isa<CallNode>(n))
				continue;
			Node* m = memoryInputOf(n);
			if(!m)
				continue;
			if(detail::idGet(s.stHead, m->getId()) < 0)
				s.touchedSt.push_back((I32)m->getId());
			detail::idSet(s.stHead, m->getId(), (I32)i);
		}
		for(U32 i = 0; i < k; ++i) {
			LoadNode* ld = dyn_cast<LoadNode>(nodes[i]);
			if(!ld)
				continue;
			Node* state = ld->getMemory();
			U32 guard = 0; // chain length bound; conservatively pin if exceeded
			for(I32 wi = state ? detail::idGet(s.stHead, state->getId()) : -1; wi >= 0; ++guard) {
				Node* w = nodes[wi];
				StoreNode* st = dyn_cast<StoreNode>(w);
				if(!st || storeMayAliasLoad(aa, st, ld) || guard >= k) {
					addEdge(ld, w); // call, aliasing store, or bound hit -> pin here
					break;
				}
				wi = detail::idGet(s.stHead, w->getId()); // skip disjoint store, walk to next writer
			}
		}

		auto producerOfMem = [&](Node* m) -> Node* {
			if(!m)
				return nullptr;
			if(ProjNode* p = dyn_cast<ProjNode>(m))
				return p->getProducer();
			return m; // a store produces memory directly
		};
		for(Node* n : nodes) {
			if(!isa<StoreNode>(n) && !isa<CallNode>(n) && !isa<LoadNode>(n))
				continue;
			Node* prod = producerOfMem(memoryInputOf(n));
			if(prod && (isa<StoreNode>(prod) || isa<CallNode>(prod)))
				addEdge(prod, n);
		}

		// restore edge
		for(Node* n : nodes)
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				if(ProjNode* p = dyn_cast<ProjNode>(n->getInput(i)))
					addEdge(p->getProducer(), n);

		for(U32 i = 0; i < k; ++i) {
			Node* n = nodes[i];
			I32 d = 0;
			for(U32 j = 0, e = n->getInputCount(); j < e; ++j)
				if(local(n->getInput(j)) >= 0)
					++d;
			s.inDeg[i] += d;
		}

		auto laterId = [](const Node* a, const Node* b) { return a->getId() > b->getId(); };
		List<Node*>& ready = s.ready;
		ready.clear();
		auto push = [&](Node* n) {
			ready.push_back(n);
			std::push_heap(ready.begin(), ready.end(), laterId);
		};

		for(U32 i = 0; i < k; ++i)
			if(s.inDeg[i] == 0)
				push(nodes[i]);

		while(!ready.empty()) {
			Node* n = ready.front();
			std::pop_heap(ready.begin(), ready.end(), laterId);
			ready.pop_back();
			out.push_back(n);
			for(Node* u : n->getUsers()) {
				I32 ui = local(u);
				if(ui >= 0 && --s.inDeg[ui] == 0)
					push(u);
			}
			for(I32 e = s.succHead[local(n)]; e >= 0; e = s.succNext[e])
				if(--s.inDeg[s.succTo[e]] == 0)
					push(nodes[s.succTo[e]]);
		}

		for(U32 i = 0; i < k; ++i)
			s.localOf[nodes[i]->getId()] = -1;
		for(I32 mid : s.touchedMem)
			s.memHead[mid] = -1;
		s.touchedMem.clear();
		for(I32 sid : s.touchedSt)
			s.stHead[sid] = -1;
		s.touchedSt.clear();

		assert(out.size() == nodes.size() && "cycle in intra-block schedule");
		return out;
	}
} // namespace rat
