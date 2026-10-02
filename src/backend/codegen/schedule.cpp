#include "codegen/schedule.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"

namespace rat {
	Schedule::Schedule(const Function& fn)
	: fn(fn) {
		buildBlocks();
		List<Node*> work;
		for(Node* n : fn)
			if(isFloating(n))
				work.push_back(n);
		List<I32> early(fn.idBound(), -1);
		scheduleEarly(work, early);
		markGuarded(work);
		scheduleLate(work, early);
		buildBlockLists();
	}

	Schedule::Schedule(const Function& fn, const List<LoadNode*>& loads)
	: fn(fn) {
		buildBlocks();
		List<Node*> work = floatingCone(loads);
		List<I32> early(fn.idBound(), -1);
		scheduleEarly(work, early);
		placeLoads(work, early);
	}

	void Schedule::buildBlocks() {
		U32 count = fn.idBound();
		headIndex.assign(count, -1);
		nodeBlock.assign(count, -1);
		headMemo.assign(count, nullptr);
		collectHeads();
		buildCFG();
		computeRpo();
		computeDominators();
		computeLoops();
		computeHoistBounds();
	}

	List<Node*> Schedule::floatingCone(const List<LoadNode*>& loads) const {
		List<U8> seen(fn.idBound(), 0);
		List<Node*> work;
		for(LoadNode* l : loads) {
			seen[l->getId()] = 1;
			work.push_back(l);
		}
		for(U32 i = 0; i < work.size(); ++i)
			for(U32 k = 0, e = work[i]->getInputCount(); k < e; ++k) {
				Node* in = work[i]->getInput(k);
				if(in && isFloating(in) && !seen[in->getId()]) {
					seen[in->getId()] = 1;
					work.push_back(in);
				}
			}
		std::reverse(work.begin(), work.end()); // inputs first, the early fixpoint settles sooner
		return work;
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
				headIndex[n->getId()] = b;
				blocks.emplace_back();
				blocks.back().head = n;
				if(ProjNode* p = dyn_cast<ProjNode>(n))
					if(isa<StartNode>(p->getProducer()))
						entryBlock = b;
			}
		}
		assert(entryBlock >= 0 && "no entry block found");
	}

	I32 Schedule::blockOfHead(const Node* head) const { return head ? headIndex[head->getId()] : -1; }

	I32 Schedule::headBlock(const Node* head) const {
		I32 b = blockOfHead(head);
		assert(b >= 0 && "control node has no block");
		return b;
	}

	Node* Schedule::headOf(Node* ctrl) const {
		List<Node*> path;
		Node* c = ctrl;
		while(true) {
			if(Node* memo = headMemo[c->getId()]) {
				c = memo;
				break;
			}
			if(isHeadNode(c))
				break;
			path.push_back(c);
			ProjNode* p = cast<ProjNode>(c);
			c = p->getProducer()->getControlInput();
		}
		for(Node* n : path)
			headMemo[n->getId()] = c;
		return c;
	}

	void Schedule::buildCFG() {
		for(I32 b = 0; b < (I32)blocks.size(); ++b) {
			setTerminator(b, walkBlock(b));
			for(U32 i = 0, e = succCount(b); i < e; ++i)
				blocks[succAt(b, i)].preds.push_back(b);
		}
	}

	Schedule::BlockEnd Schedule::walkBlock(I32 b) {
		Node* cur = blocks[b].head;
		while(true) {
			BlockEnd end;
			Node* nextCall = nullptr;
			for(Node* u : cur->getUsers()) {
				switch(u->getOpcode()) {
				case Opcode::Store:
				case Opcode::StackAlloc:
				case Opcode::StackSave:
				case Opcode::StackRestore:
					if(u->getControlInput() == cur)
						nodeBlock[u->getId()] = b;
					break;
				case Opcode::Call:
				case Opcode::Asm:
					if(u->getControlInput() == cur) {
						nodeBlock[u->getId()] = b;
						nextCall = u;
					}
					break;
				case Opcode::If:
				case Opcode::Switch:
					if(u->getControlInput() == cur)
						end.ifTerm = u;
					break;
				case Opcode::Return:
					if(u->getControlInput() == cur)
						end.retTerm = u;
					break;
				case Opcode::Region:
					for(U32 k = 0, e = u->getInputCount(); k < e; ++k)
						if(u->getInput(k) == cur) {
							end.gotoRegion = u;
							end.gotoIdx = (I32)k;
						}
					break;
				default:
					break;
				}
			}
			if(!nextCall)
				return end;
			cur = requireProj(nextCall, CallNode::controlProjIndex());
		}
	}

	void Schedule::setTerminator(I32 b, const BlockEnd& end) {
		Block& t = blocks[b];
		if(SwitchNode* sw = dyn_cast<SwitchNode>(end.ifTerm)) {
			t.term = TermKind::Switch;
			t.termNode = sw;
			for(U32 k = 0, e = sw->getSlotCount(); k < e; ++k)
				t.caseB.push_back(headBlock(requireProj(sw, k)));
		} else if(end.ifTerm) {
			t.term = TermKind::Branch;
			t.termNode = end.ifTerm;
			t.thenB = headBlock(requireProj(end.ifTerm, IfNode::thenProjIndex()));
			t.elseB = headBlock(requireProj(end.ifTerm, IfNode::elseProjIndex()));
		} else if(end.retTerm) {
			t.term = TermKind::Return;
			t.termNode = end.retTerm;
		} else {
			assert(end.gotoRegion && "block has no terminator");
			t.term = TermKind::Goto;
			t.gotoB = headBlock(end.gotoRegion);
			t.gotoPredIdx = end.gotoIdx;
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

	List<I32> Schedule::successors(I32 b) const {
		List<I32> out;
		for(U32 i = 0, e = succCount(b); i < e; ++i)
			out.push_back(succAt(b, i));
		return out;
	}

	void Schedule::computeRpo() {
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
	}

	void Schedule::computeDominators() {
		blocks[entryBlock].idom = entryBlock;
		B32 changed = true;
		while(changed) {
			changed = false;
			for(I32 b : rpoOrder) {
				if(b == entryBlock)
					continue;
				I32 newIdom = -1;
				for(I32 p : blocks[b].preds) {
					if(blocks[p].idom == -1)
						continue;
					newIdom = (newIdom == -1) ? p : intersect(p, newIdom);
				}
				if(newIdom != -1 && blocks[b].idom != newIdom) {
					blocks[b].idom = newIdom;
					changed = true;
				}
			}
		}
		for(I32 b : rpoOrder)
			blocks[b].domDepth = (b == entryBlock) ? 0 : blocks[blocks[b].idom].domDepth + 1;
	}

	I32 Schedule::intersect(I32 a, I32 b) const {
		while(a != b) {
			while(post[a] < post[b])
				a = blocks[a].idom;
			while(post[b] < post[a])
				b = blocks[b].idom;
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

	// a hoist only ever moves a node to a shallower loop, so record how shallow the
	// idom path gets: straight-line code then never has to walk it
	void Schedule::computeHoistBounds() {
		for(I32 b : rpoOrder) {
			Block& bl = blocks[b];
			bl.minDepthAbove = bl.loopDepth;
			if(b != entryBlock)
				bl.minDepthAbove = std::min(bl.loopDepth, blocks[bl.idom].minDepthAbove);
		}
	}

	B32 Schedule::isFloating(const Node* n) {
		Opcode op = n->getOpcode();
		if(op == Opcode::Alloc)
			return false;
		if(op == Opcode::Global)
			return true;
		if(op == Opcode::Constant)
			return n->getType()->isFloat() && n->getType()->getFloatWidth() != 128;
		return op == Opcode::Load || op == Opcode::Select || isArithmeticOpcode(op) ||
					 isVectorUtilOpcode(op);
	}

	B32 Schedule::mayTrap(const Node* n) {
		Opcode op = n->getOpcode();
		if(op == Opcode::Load)
			return true;
		if(op != Opcode::SDiv && op != Opcode::UDiv && op != Opcode::SRem && op != Opcode::URem)
			return false;
		const ConstantNode* c = dyn_cast<ConstantNode>(cast<BinaryNode>(n)->getRHS());
		if(!c)
			return true;
		I64 d = signExtend(c->getValue(), n->getType()->getIntWidth());
		B32 isSigned = op == Opcode::SDiv || op == Opcode::SRem;
		return d == 0 || (isSigned && d == -1); // INT_MIN / -1 traps
	}

	void Schedule::markGuarded(const List<Node*>& work) {
		guarded.assign(fn.idBound(), 0);
		List<Node*> stack;
		for(Node* n : work)
			if(mayTrap(n))
				stack.push_back(n);
		while(!stack.empty()) {
			Node* n = stack.back();
			stack.pop_back();
			if(guarded[n->getId()] || isa<LoadNode>(n) || !isFloating(n))
				continue;
			guarded[n->getId()] = 1;
			for(Node* u : n->getUsers())
				stack.push_back(u);
		}
	}

	I32 Schedule::homeBlock(Node* n) const { return headBlock(headOf(n->getControlInput())); }

	I32 Schedule::hoistTarget(const Node* n, I32 late, I32 early) const {
		if(blocks[late].minDepthAbove >= blocks[late].loopDepth)
			return late; // nothing above is shallower, so the walk cannot move it
		Opcode op = n->getOpcode();
		B32 remat = op == Opcode::Constant || op == Opcode::Global;
		B32 trapping = mayTrap(n) || (n->getId() < guarded.size() && guarded[n->getId()]);
		I32 cur = late, pick = late;
		while(true) {
			if(blocks[cur].loopDepth < blocks[pick].loopDepth) {
				pick = cur;
				if(remat)
					break; // first depth drop only
			}
			if(cur == early || cur == entryBlock)
				break;
			I32 next = blocks[cur].idom;
			if(next == cur)
				break;
			if(trapping && succCount(next) != 1)
				break;
			cur = next;
		}
		return pick;
	}

	I32 Schedule::fixedDataBlock(Node* n, const List<I32>& early) const {
		if(isFloating(n))
			return early[n->getId()];
		switch(n->getOpcode()) {
		case Opcode::Phi:
			return blockOfHead(cast<PhiNode>(n)->getRegion());
		case Opcode::Store:
		case Opcode::Call:
		case Opcode::Asm:
		case Opcode::StackAlloc:
		case Opcode::StackSave:
		case Opcode::StackRestore:
			return blockOf(n);
		case Opcode::Proj: {
			Node* prod = cast<ProjNode>(n)->getProducer();
			if(isa<CallNode>(prod) || isa<AsmNode>(prod))
				return blockOf(prod); // call/asm value/control projection
			return -1;
		}
		default:
			return -1; // no constraint
		}
	}

	void Schedule::scheduleEarly(const List<Node*>& work, List<I32>& early) {
		for(Node* n : work)
			early[n->getId()] = entryBlock;

		// deepest input block, to fixpoint (a floating input may not be settled)
		B32 changed = true;
		while(changed) {
			changed = false;
			for(Node* n : work) {
				I32 e = deepestInput(n, early);
				// a load's placement follows from its home block alone
				if(isa<LoadNode>(n))
					e = hoistTarget(n, homeBlock(n), e);
				if(early[n->getId()] != e) {
					early[n->getId()] = e;
					changed = true;
				}
			}
		}
	}

	I32 Schedule::deepestInput(const Node* n, const List<I32>& early) const {
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
		return e;
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
		I32 b = nodeBlock[u->getId()];
		if(b >= 0)
			return b;
		if(isa<ReturnNode>(u) || isa<IfNode>(u))
			return homeBlock(u);
		return -1;
	}

	I32 Schedule::predBlockForRegionInput(I32 rb, U32 i) const {
		return headBlock(headOf(blocks[rb].head->getInput(i)));
	}

	B32 Schedule::place(Node* n, I32 late, const List<I32>& early) {
		U32 id = n->getId();
		I32 pick = hoistTarget(n, late, early[id]);
		if(nodeBlock[id] == pick)
			return false;
		nodeBlock[id] = pick;
		return true;
	}

	void Schedule::scheduleLate(const List<Node*>& work, const List<I32>& early) {
		// late = LCA of use blocks; then hoist to the shallowest loop depth on the
		// dominator path between early and late. Iterated to a fixpoint
		B32 changed = true;
		while(changed) {
			changed = false;
			for(U32 wi = (U32)work.size(); wi > 0; --wi) {
				Node* n = work[wi - 1];
				I32 late = lateBlock(n);
				if(late >= 0 && place(n, late, early))
					changed = true;
			}
		}
	}

	I32 Schedule::lateBlock(Node* n) const {
		// floating loads move up from where they were built but never
		// below it, so the home block is a sound late bound
		if(isa<LoadNode>(n))
			return homeBlock(n);
		I32 late = -1;
		for(Node* u : n->getUsers())
			late = lca(late, useBlock(u, n));
		return late; // -1: no placed use yet
	}

	// a load's placement reads only early and its home block, never another
	// node's late block, so it is final after one pass
	void Schedule::placeLoads(const List<Node*>& work, const List<I32>& early) {
		for(Node* n : work)
			if(isa<LoadNode>(n))
				place(n, homeBlock(n), early);
	}

	I32 Schedule::blockOf(const Node* n) const { return n ? nodeBlock[n->getId()] : -1; }

	void Schedule::buildBlockLists() {
		for(Node* n : fn)
			if(PhiNode* phi = dyn_cast<PhiNode>(n))
				if(phi->getType()->isData())
					blocks[headBlock(phi->getRegion())].phis.push_back(phi);

		// bucket the placed nodes by block, in function order, without a list per block
		U32 nb = (U32)blocks.size();
		List<I32> start(nb + 1, 0);
		for(Node* n : fn) {
			I32 b = listedBlock(n);
			if(b >= 0)
				++start[b + 1];
		}
		for(U32 b = 0; b < nb; ++b)
			start[b + 1] += start[b];
		List<I32> fill(start.begin(), start.end() - 1);
		List<Node*> flat(start[nb]);
		for(Node* n : fn) {
			I32 b = listedBlock(n);
			if(b >= 0)
				flat[fill[b]++] = n;
		}

		TopoScratch scratch;
		scratch.localOf.assign(fn.idBound(), -1);
		scratch.stHead.assign(fn.idBound(), -1);
		AliasAnalysis aa(8);
		List<Node*> raw;
		for(U32 b = 0; b < nb; ++b) {
			raw.assign(flat.begin() + start[b], flat.begin() + start[b + 1]);
			blocks[b].nodes = topoOrder(raw, aa, scratch);
		}
	}

	// block of a node that goes into a block list
	I32 Schedule::listedBlock(const Node* n) const {
		B32 pinned =
				isa<StoreNode>(n) || isa<CallNode>(n) || isa<AsmNode>(n) || isStackOpcode(n->getOpcode());
		if(!pinned && !isFloating(n))
			return -1; // none
		return nodeBlock[n->getId()];
	}

	Node* Schedule::memoryInputOf(const Node* n) {
		if(const LoadNode* l = dyn_cast<LoadNode>(n))
			return l->getMemory();
		if(const StoreNode* s = dyn_cast<StoreNode>(n))
			return s->getMemory();
		if(const CallNode* c = dyn_cast<CallNode>(n))
			return c->getMemory();
		if(const AsmNode* a = dyn_cast<AsmNode>(n))
			return a->getMemory();
		if(isStackOpcode(n->getOpcode()))
			return n->getInput(1);
		return nullptr;
	}

	B32 Schedule::isMemWriter(const Node* n) {
		return isa<StoreNode>(n) || isa<CallNode>(n) || isa<AsmNode>(n) || isa<StackRestoreNode>(n);
	}

	namespace detail {
		B32 storeMayAliasLoad(const AliasAnalysis& aa, const StoreNode* st, const LoadNode* ld) {
			return aa.alias(
								 st->getPointer(), aa.getAccessSize(st), ld->getPointer(), aa.getAccessSize(ld)) !=
						 AliasResult::NoAlias;
		}

		B32 laterId(const Node* a, const Node* b) { return a->getId() > b->getId(); }
	} // namespace detail

	I32 Schedule::TopoScratch::local(const Node* n) const { return n ? localOf[n->getId()] : -1; }

	// extra ordering edges
	void Schedule::TopoScratch::addEdge(Node* before, Node* after) {
		if(before == after)
			return;
		I32 bi = local(before), ai = local(after);
		if(bi < 0 || ai < 0)
			return;
		succTo.push_back(ai);
		succNext.push_back(succHead[bi]);
		succHead[bi] = (I32)succTo.size() - 1;
		++inDeg[ai];
	}

	void Schedule::TopoScratch::push(Node* n) {
		ready.push_back(n);
		std::push_heap(ready.begin(), ready.end(), detail::laterId);
	}

	Node* Schedule::TopoScratch::pop() {
		Node* n = ready.front();
		std::pop_heap(ready.begin(), ready.end(), detail::laterId);
		ready.pop_back();
		return n;
	}

	List<Node*>
	Schedule::topoOrder(List<Node*>& nodes, const AliasAnalysis& aa, TopoScratch& s) const {
		U32 k = (U32)nodes.size();
		List<Node*> out;
		out.reserve(k);
		if(k == 0)
			return out;

		for(U32 i = 0; i < k; ++i)
			s.localOf[nodes[i]->getId()] = (I32)i;
		s.inDeg.assign(k, 0);
		s.succHead.assign(k, -1);
		s.succNext.clear();
		s.succTo.clear();
		addAntiDeps(nodes, aa, s);
		addOrderEdges(nodes, s);

		s.ready.clear();
		for(U32 i = 0; i < k; ++i)
			if(s.inDeg[i] == 0)
				s.push(nodes[i]);
		while(!s.ready.empty()) {
			Node* n = s.pop();
			out.push_back(n);
			for(Node* u : n->getUsers()) {
				I32 ui = s.local(u);
				if(ui >= 0 && --s.inDeg[ui] == 0)
					s.push(u);
			}
			for(I32 e = s.succHead[s.local(n)]; e >= 0; e = s.succNext[e])
				if(--s.inDeg[s.succTo[e]] == 0)
					s.push(nodes[s.succTo[e]]);
		}

		for(U32 i = 0; i < k; ++i)
			s.localOf[nodes[i]->getId()] = -1;
		for(I32 sid : s.touchedSt)
			s.stHead[sid] = -1;
		s.touchedSt.clear();

		assert(out.size() == nodes.size() && "cycle in intra-block schedule");
		return out;
	}

	// WAR anti-deps. a store/call/asm/stackrestore totally orders the block's memory
	void Schedule::addAntiDeps(const List<Node*>& nodes, const AliasAnalysis& aa, TopoScratch& s) {
		U32 k = (U32)nodes.size();
		for(U32 i = 0; i < k; ++i) {
			Node* m = isMemWriter(nodes[i]) ? memoryInputOf(nodes[i]) : nullptr;
			if(!m)
				continue;
			if(s.stHead[m->getId()] < 0)
				s.touchedSt.push_back((I32)m->getId());
			s.stHead[m->getId()] = (I32)i;
		}
		for(U32 i = 0; i < k; ++i) {
			LoadNode* ld = dyn_cast<LoadNode>(nodes[i]);
			if(!ld)
				continue;
			Node* state = ld->getMemory();
			U32 guard = 0; // chain length bound; conservatively pin if exceeded
			for(I32 wi = state ? s.stHead[state->getId()] : -1; wi >= 0; ++guard) {
				Node* w = nodes[wi];
				StoreNode* st = dyn_cast<StoreNode>(w);
				if(!st || detail::storeMayAliasLoad(aa, st, ld) || guard >= k) {
					s.addEdge(ld, w); // call, aliasing store, or bound hit -> pin here
					break;
				}
				wi = s.stHead[w->getId()]; // skip disjoint store, walk to next writer
			}
		}
	}

	void Schedule::addOrderEdges(const List<Node*>& nodes, TopoScratch& s) {
		for(Node* n : nodes) {
			Node* prod = memoryInputOf(n);
			if(ProjNode* p = dyn_cast<ProjNode>(prod))
				prod = p->getProducer();
			if(prod && isMemWriter(prod))
				s.addEdge(prod, n);
		}

		// every stack op moves rsp, so they run in the order they were built
		Node* prevStack = nullptr;
		for(Node* n : nodes) {
			if(!isStackOpcode(n->getOpcode()))
				continue;
			if(prevStack)
				s.addEdge(prevStack, n);
			prevStack = n;
		}

		// restore edge
		for(U32 i = 0; i < nodes.size(); ++i)
			for(U32 j = 0, e = nodes[i]->getInputCount(); j < e; ++j) {
				Node* in = nodes[i]->getInput(j);
				if(s.local(in) >= 0)
					++s.inDeg[i];
				if(ProjNode* p = dyn_cast<ProjNode>(in))
					s.addEdge(p->getProducer(), nodes[i]);
			}
	}
} // namespace rat
