#include "CodeGen/LinearScanRegAlloc.h"

#include "Target/Target.h"

namespace rat {
	void LinearScanRegAllocPass::solve() {
		pinsByPoint.clear();
		pinsByPoint.reserve(fixedAt.size());
		for(const auto& kv : fixedAt)
			pinsByPoint.emplace_back(kv.first, &kv.second);
		std::sort(pinsByPoint.begin(), pinsByPoint.end(), [](const auto& a, const auto& b) {
			return a.first < b.first;
		});

		buildIntervals();
		assignRegs();
		assignSpillSlots();
	}

	RegAllocBase::Assignment LinearScanRegAllocPass::assignmentOf(VReg v) {
		const Interval& iv = intervals[v];
		return {iv.assigned, iv.spillSlot, iv.cls, iv.spilled};
	}

	void LinearScanRegAllocPass::buildIntervals() {
		List<VRegSet> liveIn, liveOut;
		liveness(liveIn, liveOut);

		auto ivFor = [&](VReg v) -> Interval& {
			Interval& iv = intervals[v];
			if(iv.vreg == kNoVReg) {
				iv.vreg = v;
				iv.cls = classOf(v);
			}
			return iv;
		};

		// backward walk per block
		Map<VReg, I32> segEnd;
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			if(blkPts[b].empty())
				continue;
			I32 first = (I32)blkPts[b].front();
			I32 last = (I32)blkPts[b].back();

			VRegSet live = liveOut[b];
			segEnd.clear();
			live.forEach([&](VReg v) { segEnd[v] = last; });

			for(I32 i = (I32)fn->blocks[b].insts.size() - 1; i >= 0; --i) {
				I32 pt = (I32)blkPts[b][(U32)i];
				const MachineInstr& in = fn->blocks[b].insts[(U32)i];
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						if(live.test(d.vreg)) {
							ivFor(d.vreg).segs.push_back({pt, segEnd[d.vreg]});
							live.reset(d.vreg);
						} else {
							ivFor(d.vreg).segs.push_back({pt, pt}); // dead def still occupies its point
						}
					}
				for(const MachineOperand& u : in.uses)
					if(u.isVReg() && !live.test(u.vreg)) {
						live.set(u.vreg);
						segEnd[u.vreg] = pt;
					}
			}
			live.forEach([&](VReg v) { ivFor(v).segs.push_back({first, segEnd[v]}); });
		}

		for(auto& kv : intervals) {
			Interval& iv = kv.second;
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
		std::sort(iv.segs.begin(), iv.segs.end(), [](const Seg& a, const Seg& b) {
			return a.start != b.start ? a.start < b.start : a.end < b.end;
		});
		List<Seg> out;
		for(const Seg& sg : iv.segs) {
			if(!out.empty() && sg.start <= out.back().end + 1)
				out.back().end = std::max(out.back().end, sg.end);
			else
				out.push_back(sg);
		}
		iv.segs = std::move(out);
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

	Set<PhysReg> LinearScanRegAllocPass::forbidden(const Interval& iv) const {
		Set<PhysReg> bad;
		for(const Seg& sg : iv.segs) {
			auto lo = std::lower_bound(pinsByPoint.begin(),
																 pinsByPoint.end(),
																 sg.start,
																 [](const auto& a, I32 pt) { return a.first < pt; });
			for(auto it = lo; it != pinsByPoint.end() && it->first <= sg.end; ++it)
				for(PhysReg p : *it->second)
					if(!pinExempt(iv.vreg, it->first, p))
						bad.insert(p);
		}
		return bad;
	}

	void LinearScanRegAllocPass::assignRegs() {
		List<Interval*> sorted;
		for(auto& kv : intervals)
			sorted.push_back(&kv.second);
		std::sort(sorted.begin(), sorted.end(), [](const Interval* a, const Interval* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});

		List<Interval*> active;
		Map<U32, Set<PhysReg>> freeRegs;
		for(const RegClass& rc : ri->classes)
			for(PhysReg p : rc.allocatable)
				freeRegs[rc.id].insert(p);

		auto expire = [&](I32 start) {
			List<Interval*> stillActive;
			List<const Interval*> expired;
			for(Interval* a : active) {
				if(a->end < start)
					expired.push_back(a);
				else
					stillActive.push_back(a);
			}
			active = std::move(stillActive);
			// move partners may share a register, it becomes free only when its last active holder
			// expires
			for(const Interval* e : expired) {
				if(e->spilled || e->assigned == kNoReg)
					continue;
				B32 stillHeld = false;
				for(const Interval* a : active)
					if(a->assigned == e->assigned && !a->spilled) {
						stillHeld = true;
						break;
					}
				if(!stillHeld)
					freeRegs[e->cls].insert(e->assigned);
			}
		};

		for(Interval* iv : sorted) {
			expire(iv->start);

			const RegClass& rc = regClass(iv->cls);
			Set<PhysReg>& pool = freeRegs[iv->cls];
			Set<PhysReg> bad = forbidden(*iv);

			// biased pick
			PhysReg pick = kNoReg;
			auto colorOf = [&](VReg p) {
				auto it = intervals.find(p);
				return it == intervals.end() ? kNoReg : it->second.assigned;
			};
			for(PhysReg h : hintedRegs(iv->vreg, colorOf)) {
				if(bad.count(h))
					continue;
				if(iv->crossesCall && !isCalleeSaved(rc, h))
					continue;
				if(isCalleeSaved(rc, h) && !usedCallee.count(h))
					continue;
				if(pool.count(h)) {
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
						if(pool.count(p) && !bad.count(p)) {
							pick = p;
							break;
						}
				} else {
					// short-lived value
					for(PhysReg p : rc.allocatable)
						if(pool.count(p) && !bad.count(p)) {
							pick = p;
							break;
						}
				}
			}

			if(pick != kNoReg) {
				pool.erase(pick);
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
		Set<PhysReg> bad = forbidden(*cur);
		Interval* victim = nullptr;
		for(Interval* a : active)
			if(a->cls == cur->cls && !a->spilled) {
				if(cur->crossesCall && !isCalleeSaved(rc, a->assigned))
					continue;
				if(bad.count(a->assigned))
					continue;
				B32 shared = false;
				for(const Interval* o : active)
					if(o != a && o->assigned == a->assigned && !o->spilled) {
						shared = true;
						break;
					}
				if(shared)
					continue;
				if(!victim || a->end > victim->end)
					victim = a;
			}

		if(victim && victim->end > cur->end) {
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
		for(auto& kv : intervals)
			if(kv.second.spilled)
				spilled.push_back(&kv.second);
		std::sort(spilled.begin(), spilled.end(), [](const Interval* a, const Interval* b) {
			return a->start != b->start ? a->start < b->start : a->vreg < b->vreg;
		});
		for(Interval* iv : spilled)
			iv->spillSlot = takeSpillSlot(iv->cls, iv->start, iv->end);
	}

} // namespace rat
