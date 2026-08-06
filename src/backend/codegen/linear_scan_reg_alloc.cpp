#include "codegen/linear_scan_reg_alloc.h"

#include "target/target.h"

namespace rat {
	void LinearScanRegAllocPass::solve() {
		pinsByPoint.clear();
		for(U32 pt = 0; pt < (U32)fixedAt.size(); ++pt)
			if(fixedAt[pt])
				pinsByPoint.emplace_back((I32)pt, fixedAt[pt]);

		buildIntervals();
		assignRegs();
		assignSpillSlots();
	}

	RegAllocBase::Assignment LinearScanRegAllocPass::assignmentOf(VReg v) {
		if(const Interval* iv = ivFind(v))
			return {iv->assigned, iv->spillSlot, iv->cls, iv->spilled};
		return {kNoReg, 0, 0, false};
	}

	void LinearScanRegAllocPass::buildIntervals() {
		liveness(liveIn, liveOut);

		if(intervals.size() < fn->nextVReg)
			intervals.resize(fn->nextVReg);
		for(U32 i = 0; i < fn->nextVReg; ++i)
			intervals[i].reset();

		auto ivFor = [&](VReg v) -> Interval& {
			Interval& iv = intervals[v];
			if(iv.vreg == kNoVReg) {
				iv.vreg = v;
				iv.cls = classOf(v);
			}
			return iv;
		};

		// backward walk per block
		segEnd.assign(fn->nextVReg, 0);
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			if(blkPts[b].empty())
				continue;
			I32 first = (I32)blkPts[b].front();
			I32 last = (I32)blkPts[b].back();

			live.copyFrom(liveOut[b]);
			live.forEach([&](VReg v) { segEnd[v] = last; });

			for(I32 i = (I32)fn->blocks[b].insts.size() - 1; i >= 0; --i) {
				I32 pt = (I32)blkPts[b][(U32)i];
				const MachineInstr& in = fn->blocks[b].insts[(U32)i];
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						++ivFor(d.vreg).uses;
						if(live.test(d.vreg)) {
							ivFor(d.vreg).segs.push_back({pt, segEnd[d.vreg]});
							live.reset(d.vreg);
						} else {
							ivFor(d.vreg).segs.push_back({pt, pt}); // dead def still occupies its point
						}
					}
				for(const MachineOperand& u : in.uses)
					if(u.isVReg()) {
						++ivFor(u.vreg).uses;
						if(!live.test(u.vreg)) {
							live.set(u.vreg);
							segEnd[u.vreg] = pt;
						}
					}
			}
			live.forEach([&](VReg v) { ivFor(v).segs.push_back({first, segEnd[v]}); });
		}

		for(Interval& iv : intervals) {
			if(!iv.live())
				continue;
			coalesceSegs(iv);
			for(I32 c : callPts) {
				for(const Seg& sg : iv.segs)
					if(sg.start < c && c < sg.end) {
						iv.crossesCall = true;
						break;
					}
				if(iv.crossesCall)
					break;
			}
		}
	}

	void LinearScanRegAllocPass::coalesceSegs(Interval& iv) {
		if(iv.segs.empty())
			return;
		std::sort(iv.segs.begin(), iv.segs.end(), [](const Seg& a, const Seg& b) {
			return a.start != b.start ? a.start < b.start : a.end < b.end;
		});
		// merge in place
		U32 w = 0;
		for(U32 r = 0; r < iv.segs.size(); ++r) {
			const Seg sg = iv.segs[r];
			if(w && sg.start <= iv.segs[w - 1].end + 1)
				iv.segs[w - 1].end = std::max(iv.segs[w - 1].end, sg.end);
			else
				iv.segs[w++] = sg;
		}
		iv.segs.resize(w);
		iv.start = iv.segs.front().start;
		iv.end = iv.segs.back().end;
	}

	B32 LinearScanRegAllocPass::coversPoint(const Interval& iv, I32 pt) {
		for(const Seg& sg : iv.segs) {
			if(sg.start > pt)
				return false;
			if(pt <= sg.end)
				return true;
		}
		return false;
	}

	B32 LinearScanRegAllocPass::overlapOnlyAt(const Interval& a,
																						const Interval& b,
																						const List<I32>& pts) {
		U32 i = 0, j = 0;
		while(i < (U32)a.segs.size() && j < (U32)b.segs.size()) {
			I32 lo = std::max(a.segs[i].start, b.segs[j].start);
			I32 hi = std::min(a.segs[i].end, b.segs[j].end);
			if(lo <= hi) {
				if(lo != hi)
					return false; // a real overlapping range, not a single touch point
				B32 isCopyPt = false;
				for(I32 p : pts)
					if(p == lo)
						isCopyPt = true;
				if(!isCopyPt)
					return false;
			}
			if(a.segs[i].end < b.segs[j].end)
				++i;
			else
				++j;
		}
		return true;
	}

	U64 LinearScanRegAllocPass::forbidden(const Interval& iv) const {
		U64 bad = 0;
		for(const Seg& sg : iv.segs) {
			auto lo = std::lower_bound(pinsByPoint.begin(),
																 pinsByPoint.end(),
																 sg.start,
																 [](const auto& a, I32 pt) { return a.first < pt; });
			for(auto it = lo; it != pinsByPoint.end() && it->first <= sg.end; ++it)
				for(U64 m = it->second; m;) {
					PhysReg p = (PhysReg)countTrailingZeros64(m);
					m &= m - 1;
					if(!pinExempt(iv.vreg, it->first, p))
						bad |= (U64)1 << p;
				}
		}
		return bad;
	}

	void LinearScanRegAllocPass::assignRegs() {
		List<Interval*> sorted;
		sorted.reserve(intervals.size());
		for(Interval& iv : intervals)
			if(iv.live())
				sorted.push_back(&iv);
		std::sort(sorted.begin(), sorted.end(), [](const Interval* a, const Interval* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});

		List<Interval*> active;
		U32 maxCls = 0;
		for(const RegClass& rc : ri->classes)
			maxCls = std::max(maxCls, rc.id);
		freeRegs.assign(maxCls + 1, 0);
		for(const RegClass& rc : ri->classes)
			for(PhysReg p : rc.allocatable)
				freeRegs[rc.id] |= (U64)1 << p;

		auto expire = [&](I32 start) {
			// compact in place
			expiredBuf.clear();
			U32 w = 0;
			for(U32 r = 0; r < active.size(); ++r) {
				Interval* a = active[r];
				if(a->end < start)
					expiredBuf.push_back(a);
				else
					active[w++] = a;
			}
			active.resize(w);
			// move partners may share a register, it becomes free only when its last active holder
			// expires
			for(const Interval* e : expiredBuf) {
				if(e->spilled || e->assigned == kNoReg)
					continue;
				B32 stillHeld = false;
				for(const Interval* a : active)
					if(a->assigned == e->assigned && !a->spilled) {
						stillHeld = true;
						break;
					}
				if(!stillHeld)
					freeRegs[e->cls] |= (U64)1 << e->assigned;
			}
		};

		for(Interval* iv : sorted) {
			expire(iv->start);

			const RegClass& rc = regClass(iv->cls);
			U64& pool = freeRegs[iv->cls];
			U64 bad = forbidden(*iv);

			// biased pick
			PhysReg pick = kNoReg;
			auto colorOf = [&](VReg p) {
				const Interval* pi = ivFind(p);
				return pi ? pi->assigned : kNoReg;
			};
			for(PhysReg h : hintedRegs(iv->vreg, colorOf)) {
				if((bad >> h) & 1)
					continue;
				if(iv->crossesCall && !isCalleeSaved(rc, h))
					continue;
				if(isCalleeSaved(rc, h) && !usedCallee.count(h))
					continue;
				if((pool >> h) & 1) {
					pick = h;
					break;
				}
				B32 shareable = false;
				for(const Interval* a : active)
					if(a->assigned == h && !a->spilled) {
						shareable =
								a->cls == iv->cls && overlapOnlyAt(*iv, *a, copyPointsBetween(iv->vreg, a->vreg));
						if(!shareable)
							break;
					}
				if(shareable) {
					pick = h;
					break;
				}
			}

			if(pick == kNoReg) {
				if(iv->crossesCall) {
					for(PhysReg p : rc.calleeSaved)
						if(((pool >> p) & 1) && !((bad >> p) & 1)) {
							pick = p;
							break;
						}
				} else {
					// short-lived value
					for(PhysReg p : rc.allocatable)
						if(((pool >> p) & 1) && !((bad >> p) & 1)) {
							pick = p;
							break;
						}
				}
			}

			if(pick != kNoReg) {
				pool &= ~((U64)1 << pick);
				iv->assigned = pick;
				if(isCalleeSaved(rc, pick))
					usedCallee.insert(pick);
				active.push_back(iv);
			} else {
				spillAt(iv, active);
			}
		}
	}

	void LinearScanRegAllocPass::spillAt(Interval* cur, List<Interval*>& active) {
		const RegClass& rc = regClass(cur->cls);
		U64 bad = forbidden(*cur);

		// spill cost ~ memory ops added: one reload per use; remat values reload
		// without a store so cost far less
		auto costOf = [&](const Interval* iv) -> F64 {
			F64 c = (F64)(iv->uses ? iv->uses : 1);
			if(rematDef.find(iv->vreg) != rematDef.end())
				c *= 0.3;
			return c;
		};

		Interval* victim = nullptr;
		for(Interval* a : active)
			if(a->cls == cur->cls && !a->spilled) {
				if(cur->crossesCall && !isCalleeSaved(rc, a->assigned))
					continue;
				if((bad >> a->assigned) & 1)
					continue;
				B32 shared = false;
				for(const Interval* o : active)
					if(o != a && o->assigned == a->assigned && !o->spilled) {
						shared = true;
						break;
					}
				if(shared)
					continue;
				if(!victim || costOf(a) < costOf(victim) ||
					 (costOf(a) == costOf(victim) && a->end > victim->end))
					victim = a;
			}

		if(victim && costOf(victim) < costOf(cur)) {
			cur->assigned = victim->assigned;
			if(isCalleeSaved(rc, cur->assigned))
				usedCallee.insert(cur->assigned);
			victim->assigned = kNoReg;
			victim->spilled = true;
			for(Interval*& a : active)
				if(a == victim)
					a = cur;
		} else {
			cur->spilled = true;
		}
	}

	void LinearScanRegAllocPass::assignSpillSlots() {
		// pack
		List<Interval*> spilled;
		for(Interval& iv : intervals)
			if(iv.live() && iv.spilled)
				spilled.push_back(&iv);
		std::sort(spilled.begin(), spilled.end(), [](const Interval* a, const Interval* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});
		for(Interval* iv : spilled)
			iv->spillSlot = takeSpillSlot(iv->cls, iv->start, iv->end);
	}

} // namespace rat
