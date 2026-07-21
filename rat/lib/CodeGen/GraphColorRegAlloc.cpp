#include "CodeGen/GraphColorRegAlloc.h"

#include "Target/Target.h"

#include <cstdlib>

namespace rat {
	void GraphColorRegAllocPass::solve() {
		buildInterference();
		simplify();
		selectColors();
	}

	RegAllocBase::Assignment GraphColorRegAllocPass::assignmentOf(VReg v) {
		const Node& n = nodes[findRep(v)];
		return {n.color, n.spillSlot, n.cls, n.spilled};
	}

	VReg GraphColorRegAllocPass::findRep(VReg v) {
		while(v < (VReg)aliasTo.size() && aliasTo[v] != v)
			v = aliasTo[v];
		return v;
	}

	GraphColorRegAllocPass::Node& GraphColorRegAllocPass::nodeFor(VReg v) {
		if(v >= (VReg)nodes.size())
			nodes.resize(v + 1);
		Node& n = nodes[v];
		if(n.vreg == kNoVReg) {
			n.vreg = v;
			n.cls = classOf(v);
		}
		return n;
	}

	void GraphColorRegAllocPass::addHalfEdge(Node& n, VReg other) {
		if(n.mustSpill)
			return;
		if(n.adj.insert(other).second) {
			++n.degree;
			if(n.adj.size() >= kAdjCap) {
				n.mustSpill = true;
				n.adj.clear();
			}
		}
	}

	void GraphColorRegAllocPass::addEdge(VReg a, VReg b) {
		if(a == b)
			return;
		Node& na = nodeFor(a);
		Node& nb = nodeFor(b);
		if(na.cls != nb.cls)
			return; // diff classes never share physical registers
		addHalfEdge(na, b);
		addHalfEdge(nb, a);
	}

	void GraphColorRegAllocPass::interfereAll(const VRegSet& live) {
		scratchVRegs.clear();
		live.forEach([&](VReg v) { scratchVRegs.push_back(v); });
		for(U32 i = 0; i < (U32)scratchVRegs.size(); ++i)
			for(U32 j = i + 1; j < (U32)scratchVRegs.size(); ++j)
				addEdge(scratchVRegs[i], scratchVRegs[j]);
	}

	void GraphColorRegAllocPass::buildInterference() {
		nodes.resize(fn->nextVReg);
		List<VRegSet> liveIn, liveOut;
		liveness(liveIn, liveOut);

		auto extend = [&](VReg v, I32 pt) {
			Node& n = nodeFor(v);
			if(n.start < 0 || pt < n.start)
				n.start = pt;
			if(pt > n.end)
				n.end = pt;
		};

		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			if(blkPts[b].empty())
				continue;
			I32 first = (I32)blkPts[b].front();
			I32 last = (I32)blkPts[b].back();
			liveIn[b].forEach([&](VReg v) { extend(v, first); });
			liveOut[b].forEach([&](VReg v) { extend(v, last); });
			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				I32 pt = (I32)blkPts[b][i];
				const MachineInstr& in = fn->blocks[b].insts[i];
				for(const MachineOperand& u : in.uses)
					if(u.isVReg()) {
						++nodeFor(u.vreg).uses;
						extend(u.vreg, pt);
					}
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						++nodeFor(d.vreg).uses;
						extend(d.vreg, pt);
					}
			}
		}

		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			VRegSet live = liveOut[b];
			interfereAll(live);
			for(I32 i = (I32)fn->blocks[b].insts.size() - 1; i >= 0; --i) {
				const MachineInstr& in = fn->blocks[b].insts[(U32)i];

				// Chaitin
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						if(nodeFor(d.vreg).mustSpill)
							continue;
						scratchVRegs.clear();
						live.forEach([&](VReg v) { scratchVRegs.push_back(v); });
						for(VReg l : scratchVRegs) {
							if(l != d.vreg)
								addEdge(d.vreg, l);
							if(nodeFor(d.vreg).mustSpill)
								break;
						}
					}

				// def kills the value
				for(const MachineOperand& d : in.defs)
					if(d.isVReg())
						live.reset(d.vreg);
				for(const MachineOperand& u : in.uses)
					if(u.isVReg())
						live.set(u.vreg);
			}

		}

		// callPts is built in point order; a node crosses a call iff some call
		// point lies strictly inside its span
		for(Node& n : nodes) {
			if(n.vreg == kNoVReg)
				continue;
			auto it = std::upper_bound(callPts.begin(), callPts.end(), n.start);
			if(it != callPts.end() && *it < n.end)
				n.crossesCall = true;
		}

		computeForbidden();
		coalesce();
	}

	void GraphColorRegAllocPass::coalesce() {
		aliasTo.resize(nodes.size());
		for(VReg v = 0; v < (VReg)aliasTo.size(); ++v)
			aliasTo[v] = v;

		// deterministic pair order: (min, max) sorted, deduplicated
		List<std::pair<VReg, VReg>> pairs;
		for(const auto& kv : copyHints)
			for(const CopyHint& h : kv.second) {
				VReg a = kv.first < h.partner ? kv.first : h.partner;
				VReg b = kv.first < h.partner ? h.partner : kv.first;
				pairs.emplace_back(a, b);
			}
		std::sort(pairs.begin(), pairs.end());
		pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

		for(auto [pa, pb] : pairs) {
			VReg a = findRep(pa), b = findRep(pb);
			if(a == b || a >= (VReg)nodes.size() || b >= (VReg)nodes.size())
				continue;
			Node& na = nodes[a];
			Node& nb = nodes[b];
			if(na.vreg == kNoVReg || nb.vreg == kNoVReg || na.cls != nb.cls)
				continue;
			if(na.mustSpill || nb.mustSpill)
				continue;
			if(na.adj.count(b))
				continue; // live ranges interfere: the copy is real

			//  briggs
			U32 k = colorCount(na.cls);
			Set<VReg> unionAdj = na.adj;
			for(VReg t : nb.adj)
				unionAdj.insert(t);
			unionAdj.erase(a);
			unionAdj.erase(b);
			if(unionAdj.size() >= kAdjCap)
				continue;
			U32 sig = 0;
			for(VReg t : unionAdj) {
				VReg rt = findRep(t);
				if(rt < (VReg)nodes.size() && nodes[rt].vreg != kNoVReg &&
					 (U32)nodes[rt].adj.size() >= k)
					++sig;
			}
			if(sig >= k)
				continue;

			// merge b into a: rewrite neighbor edges, union constraints and span
			for(VReg t : nb.adj) {
				VReg rt = findRep(t);
				if(rt >= (VReg)nodes.size() || nodes[rt].vreg == kNoVReg)
					continue;
				nodes[rt].adj.erase(b);
				if(rt != a)
					nodes[rt].adj.insert(a);
			}
			na.adj = std::move(unionAdj);
			na.degree = (U32)na.adj.size();
			na.uses += nb.uses;
			if(nb.start >= 0 && (na.start < 0 || nb.start < na.start))
				na.start = nb.start;
			if(nb.end > na.end)
				na.end = nb.end;
			na.forbidden |= nb.forbidden;
			na.crossesCall = na.crossesCall || nb.crossesCall;
			nb.coalesced = true;
			nb.vreg = kNoVReg; // drop from all rep-only loops
			aliasTo[b] = a;
		}
	}

	void GraphColorRegAllocPass::computeForbidden() {
		List<std::pair<I32, U64>> pins;
		pins.reserve(fixedAt.size());
		for(const auto& kv : fixedAt)
			pins.emplace_back(kv.first, kv.second);
		std::sort(pins.begin(), pins.end(), [](const auto& a, const auto& b) {
			return a.first < b.first;
		});

		for(Node& n : nodes) {
			if(n.vreg == kNoVReg)
				continue;
			auto lo = std::lower_bound(pins.begin(),
																 pins.end(),
																 n.start,
																 [](const auto& a, I32 pt) { return a.first < pt; });
			for(auto it = lo; it != pins.end() && it->first <= n.end; ++it)
				for(U64 m = it->second; m;) {
					PhysReg p = (PhysReg)countTrailingZeros64(m);
					m &= m - 1;
					if(!pinExempt(n.vreg, it->first, p))
						n.forbidden |= regBit(p);
				}

			if(n.crossesCall) {
				const RegClass& rc = regClass(n.cls);
				for(PhysReg p : rc.allocatable)
					if(!isCalleeSaved(rc, p))
						n.forbidden |= regBit(p);
			}
		}
	}

	U32 GraphColorRegAllocPass::colorCount(U32 cls) const {
		return (U32)regClass(cls).allocatable.size();
	}

	F64 GraphColorRegAllocPass::spillCost(const Node& n) const {
		U32 deg = (U32)n.adj.size();
		if(deg == 0)
			deg = 1;
		F64 span = (F64)(n.end - n.start + 1);
		if(span < 1.0)
			span = 1.0;
		return ((F64)n.uses / span) / (F64)deg;
	}

	void GraphColorRegAllocPass::simplify() {
		selectStack.clear();

		// worklist simplify
		List<U32> degree(nodes.size(), 0);
		List<B32> removed(nodes.size(), false);
		List<VReg> ready;
		U32 remaining = 0;

		for(Node& n : nodes) {
			if(n.vreg == kNoVReg)
				continue;
			if(n.mustSpill) {
				removed[n.vreg] = true;
				continue;
			}
			U32 d = 0;
			for(VReg a : n.adj)
				if(a < (VReg)nodes.size() && nodes[a].vreg != kNoVReg && !nodes[a].mustSpill)
					++d;
			degree[n.vreg] = d;
			++remaining;
			if(d < colorCount(n.cls))
				ready.push_back(n.vreg);
		}

		auto dropNode = [&](VReg v) {
			removed[v] = true;
			selectStack.push_back(v);
			--remaining;
			for(VReg a : nodes[v].adj) {
				if(a >= (VReg)nodes.size() || nodes[a].vreg == kNoVReg || removed[a])
					continue;
				if(degree[a]-- == colorCount(nodes[a].cls))
					ready.push_back(a); // just dropped below k
			}
		};

		U32 head = 0;
		while(remaining > 0) {
			if(head < ready.size()) {
				VReg v = ready[head++];
				if(removed[v])
					continue;
				dropNode(v);
				continue;
			}

			// stalled: push the cheapest spill candidate
			VReg best = kNoVReg;
			F64 bestCost = 0.0;
			for(Node& n : nodes) {
				if(n.vreg == kNoVReg || removed[n.vreg])
					continue;
				F64 c = spillCost(n);
				if(best == kNoVReg || c < bestCost) {
					best = n.vreg;
					bestCost = c;
				}
			}
			if(best == kNoVReg)
				break;
			dropNode(best);
		}
	}

	void GraphColorRegAllocPass::selectColors() {
		for(Node& n : nodes)
			if(n.vreg != kNoVReg && n.mustSpill)
				n.spilled = true;

		for(I32 i = (I32)selectStack.size() - 1; i >= 0; --i) {
			VReg v = selectStack[(U32)i];
			Node& n = nodes[v];
			const RegClass& rc = regClass(n.cls);

			U64 used = n.forbidden;
			for(VReg a : n.adj)
				if(a < (VReg)nodes.size() && nodes[a].vreg != kNoVReg && nodes[a].color != kNoReg)
					used |= regBit(nodes[a].color);

			// biased coloring: a legal hinted register elides the copy that produced the hint
			PhysReg pick = kNoReg;
			auto colorOf = [&](VReg p) {
				return p < (VReg)nodes.size() && nodes[p].vreg != kNoVReg ? nodes[p].color : kNoReg;
			};
			for(PhysReg h : hintedRegs(v, colorOf)) {
				if((used & regBit(h)) || !isAllocatable(rc, h))
					continue;
				if(n.crossesCall && !isCalleeSaved(rc, h))
					continue;
				if(isCalleeSaved(rc, h) && !usedCallee.count(h))
					continue;
				pick = h;
				break;
			}

			if(pick == kNoReg && n.crossesCall) {
				for(PhysReg p : rc.calleeSaved)
					if(!(used & regBit(p))) {
						pick = p;
						break;
					}
			}
			if(pick == kNoReg)
				for(PhysReg p : rc.allocatable)
					if(!(used & regBit(p))) {
						pick = p;
						break;
					}

			if(pick != kNoReg) {
				n.color = pick;
				if(isCalleeSaved(rc, pick))
					usedCallee.insert(pick);
			} else {
				n.spilled = true;
			}
		}

		assignSpillSlots();
	}

	void GraphColorRegAllocPass::assignSpillSlots() {
		// pack
		List<Node*> spilled;
		for(Node& n : nodes)
			if(n.vreg != kNoVReg && n.spilled)
				spilled.push_back(&n);
		std::sort(spilled.begin(), spilled.end(), [](const Node* a, const Node* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});
		for(Node* n : spilled)
			n->spillSlot = takeSpillSlot(n->cls, n->start, n->end);
	}

} // namespace rat
