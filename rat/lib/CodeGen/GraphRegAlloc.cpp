#include "CodeGen/GraphRegAlloc.h"

#include "CodeGen/MachineModule.h"
#include "IR/Module.h"
#include "Target/Target.h"

namespace rat {
	void GraphColorRegAllocPass::number() {
		blkPts.assign(fn->blocks.size(), {});
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				I32 pt = (I32)order.size();
				blkPts[b].push_back((U32)pt);
				order.push_back({b, i});
				const MachineInstr& in = fn->blocks[b].insts[i];
				if(in.isCall)
					callPts.push_back(pt);
				for(const MachineOperand& o : in.uses)
					if(o.isPhys())
						fixedAt[pt].insert(o.phys);
				for(const MachineOperand& o : in.defs)
					if(o.isPhys())
						fixedAt[pt].insert(o.phys);
				for(PhysReg p : in.clobbers)
					fixedAt[pt].insert(p);
			}
		pinFixedArgWindows();
	}

	void GraphColorRegAllocPass::pinFixedArgWindows() {
		for(I32 c : callPts) {
			U32 b = order[(U32)c].block;
			U32 callIdx = order[(U32)c].inst;
			const List<U32>& pts = blkPts[b];
			const MachineInstr& call = fn->blocks[b].insts[callIdx];
			for(const MachineOperand& u : call.uses) {
				if(!u.isPhys())
					continue;
				PhysReg p = u.phys;
				for(I32 i = (I32)callIdx - 1; i >= 0; --i) {
					I32 pt = (I32)pts[(U32)i];
					const MachineInstr& in = fn->blocks[b].insts[(U32)i];
					B32 defsP = false;
					for(const MachineOperand& d : in.defs)
						if(d.isPhys() && d.phys == p) {
							defsP = true;
							break;
						}
					fixedAt[pt].insert(p);
					if(defsP)
						break;
				}
			}
		}
	}

	void GraphColorRegAllocPass::liveness(List<Set<VReg>>& liveIn, List<Set<VReg>>& liveOut) {
		U32 nb = (U32)fn->blocks.size();
		List<Set<VReg>> useSet(nb), defSet(nb);
		for(U32 b = 0; b < nb; ++b) {
			Set<VReg> defined;
			for(const MachineInstr& in : fn->blocks[b].insts) {
				for(const MachineOperand& u : in.uses)
					if(u.isVReg() && !defined.count(u.vreg))
						useSet[b].insert(u.vreg);
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						defined.insert(d.vreg);
						defSet[b].insert(d.vreg);
					}
			}
		}
		liveIn.assign(nb, {});
		liveOut.assign(nb, {});
		B32 changed = true;
		while(changed) {
			changed = false;
			for(I32 b = (I32)nb - 1; b >= 0; --b) {
				Set<VReg> out;
				for(I32 s : fn->blocks[b].succs)
					for(VReg v : liveIn[s])
						out.insert(v);
				Set<VReg> in = useSet[b];
				for(VReg v : out)
					if(!defSet[b].count(v))
						in.insert(v);
				if(in.size() != liveIn[b].size() || out.size() != liveOut[b].size()) {
					changed = true;
					liveIn[b] = std::move(in);
					liveOut[b] = std::move(out);
				}
			}
		}
	}

	U32 GraphColorRegAllocPass::classOf(VReg v) const {
		auto it = fn->vregClass.find(v);
		return it == fn->vregClass.end() ? 0 : it->second;
	}

	const RegClass& GraphColorRegAllocPass::regClass(U32 cls) const { return ri->classes[cls]; }

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

	void GraphColorRegAllocPass::interfereAll(const Set<VReg>& live) {
		for(auto i = live.begin(); i != live.end(); ++i) {
			auto j = i;
			for(++j; j != live.end(); ++j)
				addEdge(*i, *j);
		}
	}

	void GraphColorRegAllocPass::buildInterference() {
		List<Set<VReg>> liveIn, liveOut;
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
			for(VReg v : liveIn[b])
				extend(v, first);
			for(VReg v : liveOut[b])
				extend(v, last);
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
			Set<VReg> live = liveOut[b];
			interfereAll(live);
			for(I32 i = (I32)fn->blocks[b].insts.size() - 1; i >= 0; --i) {
				const MachineInstr& in = fn->blocks[b].insts[(U32)i];

				// Chaitin
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						if(nodeFor(d.vreg).mustSpill)
							continue;
						for(VReg l : live) {
							if(l != d.vreg)
								addEdge(d.vreg, l);
							if(nodeFor(d.vreg).mustSpill)
								break;
						}
					}

				// def kills the value
				for(const MachineOperand& d : in.defs)
					if(d.isVReg())
						live.erase(d.vreg);
				for(const MachineOperand& u : in.uses)
					if(u.isVReg())
						live.insert(u.vreg);
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
		for(auto& kv : nodes) {
			Node& n = kv.second;
			if(n.mustSpill) {
				n.spilled = true;
				n.spillSlot = hooks->allocSlot(*fn, n.cls, ri->spillSlotBytes);
			}
		}

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

			PhysReg pick = kNoReg;
			if(n.crossesCall) {
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
				n.spillSlot = hooks->allocSlot(*fn, n.cls, ri->spillSlotBytes);
			}
		}
	}

	B32 GraphColorRegAllocPass::isCalleeSaved(const RegClass& rc, PhysReg p) {
		for(PhysReg c : rc.calleeSaved)
			if(c == p)
				return true;
		return false;
	}

	void GraphColorRegAllocPass::rewrite() {
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			List<MachineInstr> out;
			for(MachineInstr& in : fn->blocks[b].insts) {
				U32 useScratch = 0;
				for(MachineOperand& u : in.uses) {
					if(!u.isVReg())
						continue;
					Node& n = nodes[u.vreg];
					if(n.spilled) {
						if(in.isCall) {
							u = MachineOperand::frameSlot(n.spillSlot, u.width);
							continue;
						}
						PhysReg sc = scratchAt(n.cls, useScratch++);
						out.push_back(hooks->makeReload(sc, n.spillSlot, n.cls, u.width));
						u = MachineOperand::fixed(sc, u.width);
					} else {
						u = MachineOperand::fixed(n.color, u.width);
					}
				}

				U32 defScratch = 0;
				List<MachineInstr> spills;
				for(MachineOperand& d : in.defs) {
					if(!d.isVReg())
						continue;
					Node& n = nodes[d.vreg];
					if(n.spilled) {
						PhysReg sc = scratchAt(n.cls, defScratch++);
						spills.push_back(hooks->makeSpill(n.spillSlot, sc, n.cls, d.width));
						d = MachineOperand::fixed(sc, d.width);
					} else {
						d = MachineOperand::fixed(n.color, d.width);
					}
				}

				out.push_back(in);
				for(MachineInstr& s : spills)
					out.push_back(std::move(s));
			}
			fn->blocks[b].insts = std::move(out);
		}
	}

	PhysReg GraphColorRegAllocPass::scratchAt(U32 cls, U32 idx) {
		const RegClass& rc = ri->classes[cls];
		if(rc.scratch.empty()) {
			ok = false;
			return kNoReg;
		}
		if(idx >= rc.scratch.size()) {
			ok = false;
			idx = (U32)rc.scratch.size() - 1;
		}
		return rc.scratch[idx];
	}

	B32 GraphColorRegAllocPass::allocate(MachineFunc& f,
																			 const RegisterInfo& r,
																			 const RegAllocHooks& h,
																			 List<PhysReg>* usedCalleeSaved) {
		fn = &f;
		ri = &r;
		hooks = &h;
		order.clear();
		blkPts.clear();
		callPts.clear();
		fixedAt.clear();
		nodes.clear();
		selectStack.clear();
		usedCallee.clear();
		ok = true;

		number();
		buildInterference();
		simplify();
		selectColors();
		rewrite();

		if(usedCalleeSaved) {
			usedCalleeSaved->clear();
			for(PhysReg p : usedCallee)
				usedCalleeSaved->push_back(p);
		}
		return ok;
	}

	B32 GraphColorRegAllocPass::run(Module& module, MachineModule& mm, const TargetInfo& target) {
		U32 changed = 0;
		for(const Function* fn : module) {
			MachineFunc& mf = mm.get(fn);
			B32 allocated =
					allocate(mf, *target.registers(), target.regAllocHooks(), &mf.usedCalleeSaved);
			assert(allocated && "register allocation ran out of scratch registers");
			(void)allocated;
			++changed;
		}
		return changed != 0;
	}
} // namespace rat
