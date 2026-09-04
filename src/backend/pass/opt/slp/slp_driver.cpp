// discovery & window formation (the Slp driver)

#include "pass/opt/slp/slp_pack.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	StoreNode* SlpPackPass::soleChainSuccessor(StoreNode* s, List<LoadNode*>& observers) {
		StoreNode* succ = nullptr;
		for(Node* u : s->getUsers()) {
			if(u->getOpcode() == Opcode::Store && cast<StoreNode>(u)->getMemory() == s) {
				if(succ && succ != u)
					return nullptr;
				succ = cast<StoreNode>(u);
			} else if(u->getOpcode() == Opcode::Load && cast<LoadNode>(u)->getMemory() == s) {
				observers.push_back(cast<LoadNode>(u));
			} else if(u->getOpcode() == Opcode::Asm) {
				return nullptr; // an asm is an opaque barrier
			} else if(usesValue(u, s)) {
				return nullptr;
			}
		}
		return succ;
	}

	B32 SlpPackPass::windowAt(const Segment& seg, U32 at, WindowShape& out) {
		if(at >= seg.size())
			return false;
		const RefinedAddr& k0 = seg[at].key;
		U32 esz = k0.size;
		if(!supportedEsz(esz))
			return false;
		U32 w = laneCountFor(esz);
		if(at + w > seg.size())
			return false;
		Type* elemTy = seg[at].store->getValue()->getType();
		Node* ctrl = seg[at].store->getControl();
		I64 lo = k0.constant;
		for(U32 j = 0; j < w; ++j) {
			const StoreInfo& si = seg[at + j];
			B32 sameGroup = si.key.sameGroup(k0);
			B32 sameElem = si.store->getValue()->getType() == elemTy;
			B32 sameCtrl = si.store->getControl() == ctrl;
			if(!sameGroup || !sameElem || !sameCtrl)
				return false;
			lo = std::min(lo, si.key.constant);
		}
		// the byte offsets must fill the w lanes above lo exactly once each
		out.byOff.assign(w, nullptr);
		for(U32 j = 0; j < w; ++j) {
			U64 d = (U64)(seg[at + j].key.constant - lo);
			if(d % esz != 0 || d / esz >= w || out.byOff[d / esz])
				return false;
			out.byOff[d / esz] = &seg[at + j];
		}
		out.begin = at;
		out.w = w;
		out.elemTy = elemTy;
		out.ctrl = ctrl;
		out.lo = lo;
		return true;
	}

	B32 SlpPackPass::windowHasObs(const Segment& seg, const WindowShape& ws) {
		for(U32 j = 0; j + 1 < ws.w; ++j)
			if(!seg[ws.begin + j].observers.empty())
				return true;
		return false;
	}

	List<Node*> SlpPackPass::laneValues(const WindowShape& w) {
		List<Node*> vals;
		for(const StoreInfo* si : w.byOff)
			vals.push_back(si->store->getValue());
		return vals;
	}

	void SlpPackPass::collectInterState(const Segment& seg,
																			U32 begin,
																			U32 count,
																			Map<const Node*, List<I64>>& interWritten,
																			Set<const Node*>& obsSet) {
		// each store maps to the prefix of lane offsets stored before it in the window
		List<I64> written;
		for(U32 j = 0; j + 1 < count; ++j) {
			written.push_back(seg[begin + j].key.constant);
			interWritten.emplace(seg[begin + j].store, written);
			for(LoadNode* L : seg[begin + j].observers)
				obsSet.insert(L);
		}
	}

	SlpPackPass::Slp::Slp(
			Function& fn, const AliasAnalysis& aa, U32 ptrBytes, B32 sse41, SlpStats& stats)
	: fn(fn),
		aa(aa),
		ptrBytes(ptrBytes),
		sse41(sse41),
		stats(stats) {}

	const SlpPackPass::Slp::StoreAddr& SlpPackPass::Slp::storeAddr(StoreNode* s) {
		auto it = addrCache.find(s);
		if(it != addrCache.end())
			return it->second;
		StoreAddr a;
		U32 sz = aa.getAccessSize(s);
		if(sz)
			a.key = refineAddr(s->getPointer(), sz);
		if(a.key.valid())
			a.sig = groupSig(a.key);
		return addrCache.emplace(s, std::move(a)).first->second;
	}

	B32 SlpPackPass::Slp::disjointStores(StoreNode* a, StoreNode* b) {
		const RefinedAddr& ka = storeAddr(a).key;
		const RefinedAddr& kb = storeAddr(b).key;
		return provablyDisjoint(aa, a->getPointer(), ka, ka.size, b->getPointer(), kb, kb.size);
	}

	// skip the maximal run of stores that are disjoint from loadBase purely by object identity
	Node* SlpPackPass::Slp::skipDisjointRun(StoreNode* s, Node* loadBase) {
		Map<const Node*, Node*>& memo = skipMemo[loadBase];
		List<const Node*> run;
		Node* cur = s;
		Node* endpoint = nullptr;
		while(true) {
			StoreNode* cs = dyn_cast<StoreNode>(cur);
			if(!cs) {
				endpoint = cur; // reached function entry / a non-store producer
				break;
			}
			auto it = memo.find(cur);
			if(it != memo.end()) {
				endpoint = it->second;
				break;
			}
			if(!AliasAnalysis::distinctObjects(loadBase, storeAddr(cs).key.base)) {
				endpoint = cur; // barrier candidate: needs the exact per-offset check
				break;
			}
			run.push_back(cur);
			cur = cs->getMemory();
		}
		for(const Node* r : run)
			memo.emplace(r, endpoint);
		return endpoint;
	}

	void SlpPackPass::Slp::normalizeLoadEdges() {
		for(Node* n : fn) {
			LoadNode* l = dyn_cast<LoadNode>(n);
			if(!l)
				continue;
			U32 lsz = aa.getAccessSize(l);
			if(!lsz)
				continue;

			// loads feeding the address computation, bounded
			List<const Node*> cone;
			if(!dataCone(l->getPointer(), 64, cone))
				continue; // unbounded cone: skip
			List<const Node*> coneLoads;
			for(const Node* c : cone)
				if(isa<LoadNode>(c))
					coneLoads.push_back(c);

			RefinedAddr lk = refineAddr(l->getPointer(), lsz);
			// the object-identity fast path is exact only when the address is a plain object
			// reference (no loads in its cone, so the addrReadsState check cannot fire)
			B32 fastPath = lk.base != nullptr && coneLoads.empty() && identifiedBase(lk.base);

			Node* m = l->getMemory();
			while(StoreNode* s = dyn_cast<StoreNode>(m)) {
				if(fastPath) {
					Node* jumped = skipDisjointRun(s, lk.base);
					if(jumped != m) {
						m = jumped; // skipped a run
						continue;
					}
				}
				U32 ssz = aa.getAccessSize(s);
				if(!ssz)
					break;
				RefinedAddr sk = refineAddr(s->getPointer(), ssz);
				if(!provablyDisjoint(aa, l->getPointer(), lk, lsz, s->getPointer(), sk, ssz))
					break;
				B32 addrReadsState = false;
				for(const Node* cl : coneLoads)
					addrReadsState |= cast<LoadNode>(cl)->getMemory() == s;
				if(addrReadsState)
					break;
				m = s->getMemory();
			}
			if(m != l->getMemory())
				l->setInput(1, m); // input 1 is the memory operand
		}
	}

	// when p's only consumer is s, reorder ... -> p -> s into ... -> pp -> s -> p;
	// returns false (no swap) if p feeds anything besides s
	B32 SlpPackPass::Slp::trySwapAdjacentStores(StoreNode* s, StoreNode* p) {
		for(Node* u : p->getUsers())
			if(u != s && usesValue(u, p))
				return false;
		List<Node*> sUsers;
		for(Node* u : s->getUsers())
			sUsers.push_back(u);
		s->setInput(1, p->getMemory());
		p->setInput(1, s);
		for(Node* u : sUsers)
			if(u != p)
				rewriteInput(u, s, p);
		return true;
	}

	void SlpPackPass::Slp::normalizeStoreChains() {
		B32 progress = true;
		for(U32 round = 0; progress && round < 8; ++round) {
			progress = false;
			for(Node* n : fn) {
				StoreNode* s = dyn_cast<StoreNode>(n);
				if(!s)
					continue;
				const StoreAddr& as = storeAddr(s);
				if(!as.key.valid())
					continue;
				U32 hops = 0;
				while(hops++ < 16) {
					StoreNode* p = dyn_cast<StoreNode>(s->getMemory());
					if(!p || p->getControl() != s->getControl())
						break;
					const StoreAddr& ap = storeAddr(p);
					if(!ap.key.valid())
						break;
					if(as.sig >= ap.sig)
						break; // same group, or already canonically ordered
					if(!disjointStores(s, p))
						break;
					if(!trySwapAdjacentStores(s, p))
						break; // p has another observer
					progress = true;
				}
			}
		}
	}

	static B32 byStoreOffset(const Pair<I64, StoreNode*>& a, const Pair<I64, StoreNode*>& b) {
		return a.first < b.first;
	}

	// reorder a maximal same-group, same-control, observer-free store run into byte-offset order
	void SlpPackPass::Slp::sortDisjointRuns() {
		for(Node* n : fn) {
			StoreNode* h = dyn_cast<StoreNode>(n);
			if(!h)
				continue;
			const StoreAddr& ah = storeAddr(h);
			if(!ah.key.valid())
				continue;
			StoreNode* pred = dyn_cast<StoreNode>(h->getMemory());
			if(pred && pred->getControl() == h->getControl() && storeAddr(pred).sig == ah.sig)
				continue; // not a run head
			List<StoreNode*> run;
			List<RefinedAddr> keys;
			StoreNode* cur = h;
			RefinedAddr kc = ah.key;
			while(true) {
				run.push_back(cur);
				keys.push_back(kc);
				List<LoadNode*> obs;
				StoreNode* nx = soleChainSuccessor(cur, obs);
				if(!obs.empty() || !nx || nx->getControl() != cur->getControl())
					break;
				const StoreAddr& an = storeAddr(nx);
				if(an.sig != ah.sig || !an.key.valid())
					break;
				cur = nx;
				kc = an.key;
			}
			if(run.size() < 3)
				continue;
			StoreNode* tail = run.back();
			if(tail->getUsers().size() != 1)
				continue; // tail must have a single memory successor for the rewire to be sound
			B32 sorted = true;
			for(U32 i = 1; i < run.size(); ++i)
				if(keys[i].constant < keys[i - 1].constant)
					sorted = false;
			if(sorted)
				continue;
			B32 disjoint = true;
			for(U32 i = 0; i < run.size() && disjoint; ++i)
				for(U32 j = i + 1; j < run.size(); ++j)
					if(!provablyDisjoint(aa,
															 run[i]->getPointer(),
															 keys[i],
															 keys[i].size,
															 run[j]->getPointer(),
															 keys[j],
															 keys[j].size)) {
						disjoint = false;
						break;
					}
			if(!disjoint)
				continue;
			List<Pair<I64, StoreNode*>> ord;
			for(U32 i = 0; i < run.size(); ++i)
				ord.push_back({keys[i].constant, run[i]});
			std::sort(ord.begin(), ord.end(), byStoreOffset);
			Node* after = tail->getUsers()[0];
			Node* mIn = run[0]->getMemory();
			for(U32 i = 0; i < ord.size(); ++i)
				ord[i].second->setInput(1, i == 0 ? mIn : (Node*)ord[i - 1].second);
			rewriteInput(after, tail, ord.back().second);
		}
	}

	// control edge is the cold (scalar fallback) arm of an earlier guard
	static B32 isElseProj(const Node* c) {
		const ProjNode* p = dyn_cast<ProjNode>(c);
		return p && p->getLabel() && String(p->getLabel()) == "slp.else";
	}

	U32 SlpPackPass::Slp::processSegment(Segment& seg) {
		U32 changed = 0;
		U32 i = 0;
		while(i < seg.size()) {
			WindowShape w0;
			if(!windowAt(seg, i, w0)) {
				++i;
				continue;
			}
			B32 coldArm = isElseProj(w0.ctrl);
			if(RegionNode* r = dyn_cast<RegionNode>(w0.ctrl)) {
				coldArm = true;
				for(U32 k = 0; coldArm && k < r->getPredecessorCount(); ++k)
					coldArm = isElseProj(r->getPredecessor(k));
			}
			if(coldArm) {
				++i;
				continue;
			}
			++stats.windowsSeen;

			B32 hasObs = windowHasObs(seg, w0);
			if(hasObs && guardsDisabled()) {
				++stats.rejectedGuarded;
				++i;
				continue;
			}

			U32 consumed = 0;
			if(hasObs)
				consumed = tryGuardedRun(seg, i, w0);
			else
				consumed = tryStaticWindow(seg, i, w0);
			if(consumed) {
				++changed;
				i += consumed;
			} else {
				++i;
			}
		}
		return changed;
	}

	U32 SlpPackPass::Slp::run() {
		normalizeLoadEdges();
		normalizeStoreChains();
		sortDisjointRuns();
		Map<Node*, StoreInfo> cand = collectCandidates();
		U32 changed = 0;
		if(!cand.empty()) {
			List<Segment> segments = buildSegments(cand);
			for(Segment& seg : segments)
				changed += processSegment(seg);
		}
		changed += packReductions();
		// sweep replaced scalars and any speculative, unprofitable trees
		fn.eliminateDeadNodes();
		return changed;
	}

	Map<Node*, SlpPackPass::StoreInfo> SlpPackPass::Slp::collectCandidates() {
		Map<Node*, StoreInfo> cand;
		for(Node* n : fn) {
			StoreNode* s = dyn_cast<StoreNode>(n);
			if(!s || !packableElem(s->getValue()->getType()))
				continue;
			RefinedAddr k = refineAddr(s->getPointer(), s->getValue()->getType()->byteSize(ptrBytes));
			if(!k.valid())
				continue;
			cand[s] = {s, std::move(k), {}};
		}
		return cand;
	}

	List<SlpPackPass::Segment> SlpPackPass::Slp::buildSegments(const Map<Node*, StoreInfo>& cand) {
		List<Segment> segments;
		for(Node* n : fn) {
			auto headIt = cand.find(n);
			if(headIt == cand.end())
				continue;
			StoreNode* s = headIt->second.store;
			Node* m = s->getMemory();
			if(m && m->getOpcode() == Opcode::Store && cand.count(m))
				continue; // not a head
			Segment seg;
			StoreNode* cur = s;
			while(cur) {
				auto it = cand.find(cur);
				if(it == cand.end())
					break;
				StoreInfo info = it->second;
				cur = soleChainSuccessor(cur, info.observers);
				seg.push_back(std::move(info));
			}
			if(seg.size() >= 2)
				segments.push_back(std::move(seg));
		}
		return segments;
	}
} // namespace rat
