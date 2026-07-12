#include "CodeGen/GraphRegAlloc.h"

#include "Target/Target.h"

namespace rat {
	void GraphColorRegAllocPass::solve() {
		buildInterference();
		simplify();
		selectColors();
	}

	RegAllocBase::Assignment GraphColorRegAllocPass::assignmentOf(VReg v) {
		const Node& n = nodes[v];
		return {n.color, n.spillSlot, n.cls, n.spilled};
	}

	GraphColorRegAllocPass::Node& GraphColorRegAllocPass::nodeFor(VReg v) {
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
		List<VReg> vs;
		live.forEach([&](VReg v) { vs.push_back(v); });
		for(U32 i = 0; i < (U32)vs.size(); ++i)
			for(U32 j = i + 1; j < (U32)vs.size(); ++j)
				addEdge(vs[i], vs[j]);
	}

	void GraphColorRegAllocPass::buildInterference() {
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
						List<VReg> vs;
						live.forEach([&](VReg v) { vs.push_back(v); });
						for(VReg l : vs) {
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

		for(auto& kv : nodes) {
			Node& n = kv.second;
			for(I32 c : callPts)
				if(n.start < c && c < n.end) {
					n.crossesCall = true;
					break;
				}
		}

		computeForbidden();
	}

	void GraphColorRegAllocPass::computeForbidden() {
		for(auto& kv : nodes) {
			Node& n = kv.second;
			for(const auto& fk : fixedAt)
				if(n.start <= fk.first && fk.first <= n.end)
					for(PhysReg p : fk.second)
						if(!pinExempt(n.vreg, fk.first, p))
							n.forbidden.insert(p);

			if(n.crossesCall) {
				const RegClass& rc = regClass(n.cls);
				for(PhysReg p : rc.allocatable)
					if(!isCalleeSaved(rc, p))
						n.forbidden.insert(p);
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
		Set<VReg> removed;
		Map<VReg, U32> degree;

		// pre-spilled nodes never enter the coloring stack
		U32 colorable = 0;
		for(auto& kv : nodes) {
			if(kv.second.mustSpill) {
				removed.insert(kv.first);
				continue;
			}
			U32 d = 0;
			for(VReg a : kv.second.adj) {
				auto it = nodes.find(a);
				if(it != nodes.end() && !it->second.mustSpill)
					++d;
			}
			degree[kv.first] = d;
			++colorable;
		}

		U32 done = 0;
		while(done < colorable) {
			B32 pushed = false;

			// simplify
			for(auto& kv : nodes) {
				VReg v = kv.first;
				if(removed.count(v))
					continue;
				if(degree[v] < colorCount(kv.second.cls)) {
					removed.insert(v);
					selectStack.push_back(v);
					++done;
					for(VReg a : kv.second.adj)
						if(!removed.count(a) && degree.count(a))
							--degree[a];
					pushed = true;
				}
			}
			if(pushed)
				continue;

			// stalled
			VReg best = kNoVReg;
			F64 bestCost = 0.0;
			for(auto& kv : nodes) {
				VReg v = kv.first;
				if(removed.count(v))
					continue;
				F64 c = spillCost(kv.second);
				if(best == kNoVReg || c < bestCost) {
					best = v;
					bestCost = c;
				}
			}
			if(best == kNoVReg)
				break;
			removed.insert(best);
			selectStack.push_back(best);
			++done;
			for(VReg a : nodes[best].adj)
				if(!removed.count(a) && degree.count(a))
					--degree[a];
		}
	}

	void GraphColorRegAllocPass::selectColors() {
		for(auto& kv : nodes)
			if(kv.second.mustSpill)
				kv.second.spilled = true;

		for(I32 i = (I32)selectStack.size() - 1; i >= 0; --i) {
			VReg v = selectStack[(U32)i];
			Node& n = nodes[v];
			const RegClass& rc = regClass(n.cls);

			Set<PhysReg> used = n.forbidden;
			for(VReg a : n.adj) {
				auto it = nodes.find(a);
				if(it != nodes.end() && it->second.color != kNoReg)
					used.insert(it->second.color);
			}

			// biased coloring: a legal hinted register elides the copy that produced the hint
			PhysReg pick = kNoReg;
			auto colorOf = [&](VReg p) {
				auto it = nodes.find(p);
				return it == nodes.end() ? kNoReg : it->second.color;
			};
			for(PhysReg h : hintedRegs(v, colorOf)) {
				if(used.count(h) || !isAllocatable(rc, h))
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
					if(!used.count(p)) {
						pick = p;
						break;
					}
			}
			if(pick == kNoReg)
				for(PhysReg p : rc.allocatable)
					if(!used.count(p)) {
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
		for(auto& kv : nodes)
			if(kv.second.spilled)
				spilled.push_back(&kv.second);
		std::sort(spilled.begin(), spilled.end(), [](const Node* a, const Node* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});
		for(Node* n : spilled)
			n->spillSlot = takeSpillSlot(n->cls, n->start, n->end);
	}

} // namespace rat
