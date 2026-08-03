#include "Pass/Opt/SlpPack.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Pass/Opt/AliasAnalysis.h"
#include "Target/Target.h"

#include <algorithm>
#include <cstdlib>

namespace rat {
	B32 SlpPackPass::envFlag(const C8* name) { return std::getenv(name) != nullptr; }

	B32 SlpPackPass::shapesDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_SHAPES");
		return v;
	}

	B32 SlpPackPass::packableElem(const Type* t) {
		if(!t)
			return false;
		if(t->isInt())
			return t->getIntWidth() == 32 || t->getIntWidth() == 64;
		if(t->isFloat())
			return t->getFloatWidth() == 32 || t->getFloatWidth() == 64;
		return false;
	}

	B32 SlpPackPass::packableBinary(Opcode op, const Type* t) {
		switch(op) {
		case Opcode::Add:
		case Opcode::Sub:
		case Opcode::And:
		case Opcode::Or:
		case Opcode::Xor:
			return t->isInt();
		case Opcode::FAdd:
		case Opcode::FSub:
		case Opcode::FMul:
		case Opcode::FDiv:
			return t->isFloat();
		default:
			return false;
		}
	}

	B32 SlpPackPass::identifiedBase(const Node* n) {
		return n && (n->getOpcode() == Opcode::Alloc || n->getOpcode() == Opcode::Global);
	}

	B32 SlpPackPass::isI64(const Node* n) {
		return n->getType() && n->getType()->isInt() && n->getType()->getIntWidth() == 64;
	}

	void SlpPackPass::refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth) {
		if(scale == 0)
			return;
		if(const ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			out.constant += scale * c->getValue();
			return;
		}
		if(depth < 12) {
			if(const BinaryNode* b = dyn_cast<BinaryNode>(n)) {
				Opcode op = b->getOpcode();
				const ConstantNode* rc = dyn_cast<ConstantNode>(b->getRHS());
				if(op == Opcode::Add && rc) {
					refineTerm32(b->getLHS(), scale, out, depth + 1);
					out.constant += scale * rc->getValue();
					return;
				}
				if(op == Opcode::Sub && rc) {
					refineTerm32(b->getLHS(), scale, out, depth + 1);
					out.constant -= scale * rc->getValue();
					return;
				}
				if(op == Opcode::Mul && rc) {
					refineTerm32(b->getLHS(), scale * rc->getValue(), out, depth + 1);
					return;
				}
				if(op == Opcode::Shl && rc && rc->getValue() >= 0 && rc->getValue() < 31) {
					refineTerm32(b->getLHS(), scale << rc->getValue(), out, depth + 1);
					return;
				}
			}
		}
		out.terms.push_back({n, scale});
	}

	void SlpPackPass::refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth) {
		if(scale == 0)
			return;
		if(const ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			out.constant += scale * c->getValue();
			return;
		}
		if(const ConvertNode* cv = dyn_cast<ConvertNode>(n)) {
			const Type* st = cv->getOperand()->getType();
			if(cv->getOpcode() == Opcode::SExt && st && st->isInt() && st->getIntWidth() == 32) {
				refineTerm32(cv->getOperand(), scale, out, depth + 1);
				return;
			}
		}
		if(depth < 12 && isI64(n)) {
			if(const BinaryNode* b = dyn_cast<BinaryNode>(n)) {
				Opcode op = b->getOpcode();
				I64 k;
				auto cval = [](const Node* x, I64& v) -> B32 {
					const ConstantNode* c = dyn_cast<ConstantNode>(x);
					if(c)
						v = c->getValue();
					return c != nullptr;
				};
				if(op == Opcode::Add) {
					refineTerm(b->getLHS(), scale, out, depth + 1);
					refineTerm(b->getRHS(), scale, out, depth + 1);
					return;
				}
				if(op == Opcode::Sub) {
					refineTerm(b->getLHS(), scale, out, depth + 1);
					refineTerm(b->getRHS(), -scale, out, depth + 1);
					return;
				}
				if(op == Opcode::Mul && (cval(b->getRHS(), k) || cval(b->getLHS(), k))) {
					const Node* x = isa<ConstantNode>(b->getRHS()) ? b->getLHS() : b->getRHS();
					refineTerm(x, scale * k, out, depth + 1);
					return;
				}
				if(op == Opcode::Shl && cval(b->getRHS(), k) && k >= 0 && k < 63) {
					refineTerm(b->getLHS(), scale << k, out, depth + 1);
					return;
				}
			}
		}
		out.terms.push_back({n, scale});
	}

	SlpPackPass::RefinedAddr SlpPackPass::refineAddr(Node* addr, U32 accessBytes) {
		RefinedAddr out;
		out.size = accessBytes;
		Node* base = addr;
		U32 hops = 0;
		while(BinaryNode* b = dyn_cast<BinaryNode>(base)) {
			Opcode op = b->getOpcode();
			if(++hops > 32 || (op != Opcode::Add && op != Opcode::Sub) || !b->getType()->isPtr() ||
				 !b->getLHS()->getType()->isPtr())
				break;
			refineTerm(b->getRHS(), op == Opcode::Add ? 1 : -1, out, 0);
			base = b->getLHS();
		}
		out.base = base;
		// canonicalize: merge equal vars, drop zero scales, sort by id
		std::sort(out.terms.begin(), out.terms.end(), [](const auto& a, const auto& b) {
			return a.first->getId() < b.first->getId();
		});
		List<std::pair<const Node*, I64>> merged;
		for(const auto& t : out.terms) {
			if(!merged.empty() && merged.back().first == t.first)
				merged.back().second += t.second;
			else
				merged.push_back(t);
		}
		merged.erase(
				std::remove_if(merged.begin(), merged.end(), [](const auto& t) { return t.second == 0; }),
				merged.end());
		out.terms = std::move(merged);
		return out;
	}

	String SlpPackPass::groupSig(const RefinedAddr& k) {
		String s = std::to_string(k.base->getId());
		for(const auto& t : k.terms) {
			s += ',';
			s += std::to_string(t.first->getId());
			s += ':';
			s += std::to_string(t.second);
		}
		return s;
	}

	B32 SlpPackPass::provablyDisjoint(const AliasAnalysis& aa,
																		Node* pa,
																		const RefinedAddr& ka,
																		U32 sza,
																		Node* pb,
																		const RefinedAddr& kb,
																		U32 szb) {
		if(ka.valid() && kb.valid() && ka.base == kb.base && ka.terms == kb.terms)
			if(ka.constant + (I64)sza <= kb.constant || kb.constant + (I64)szb <= ka.constant)
				return true;
		return aa.alias(pa, sza, pb, szb) == AliasResult::NoAlias;
	}

	U64 SlpPackPass::ShapeHash::shape(const Node* n, U32 depth) {
		if(!n)
			return 0x51ab;
		if(depth == kDepth) {
			if(auto it = memo.find(n); it != memo.end())
				return it->second;
		}
		U64 h = mix((U64)n->getOpcode() << 8, n->getType()->getUid());
		if(const LoadNode* l = dyn_cast<LoadNode>(n)) {
			RefinedAddr ra = refineAddr(const_cast<LoadNode*>(l)->getPointer(), 1);
			h = mix(h, ra.base ? ra.base->getId() : 0);
			for(const auto& t : ra.terms)
				h = mix(mix(h, t.first->getId()), (U64)t.second);
		}
		if(depth && isArithmeticOpcode(n->getOpcode())) {
			U32 e = n->getInputCount();
			if(n->isCommutative() && e == 2) {
				U64 a = shape(n->getInput(0), depth - 1);
				U64 b = shape(n->getInput(1), depth - 1);
				h = mix(h, a < b ? mix(a, b) : mix(b, a));
			} else {
				for(U32 i = 0; i < e; ++i)
					h = mix(h, shape(n->getInput(i), depth - 1));
			}
		}
		if(depth == kDepth)
			memo.emplace(n, h);
		return h;
	}

	void SlpPackPass::Packer::addGuard(const RefinedAddr& k, Node* lane0Ptr, U32 bytes) {
		String sig = groupSig(k);
		for(GuardGroup& g : guardGroups)
			if(g.sig == sig) {
				if(k.constant < g.minC) {
					g.minC = k.constant;
					g.ptr = lane0Ptr;
				}
				g.maxC = std::max(g.maxC, k.constant + (I64)bytes);
				return;
			}
		guardGroups.push_back({std::move(sig), lane0Ptr, k.constant, k.constant + (I64)bytes});
	}

	B32 SlpPackPass::dataCone(const Node* root, U32 cap, List<const Node*>& out) {
		List<const Node*> work = {root};
		Set<const Node*> seen;
		while(!work.empty()) {
			const Node* c = work.back();
			work.pop_back();
			if(!c || !seen.insert(c).second)
				continue;
			if(seen.size() > cap)
				return false; // unbounded cone
			out.push_back(c);
			for(U32 i = 0, e = c->getInputCount(); i < e; ++i)
				if(const Node* in = c->getInput(i))
					if(in->getType() && in->getType()->isData())
						work.push_back(in);
		}
		return true;
	}

	B32 SlpPackPass::usesValue(const Node* u, const Node* x) {
		for(U32 i = 0, e = u->getInputCount(); i < e; ++i)
			if(u->getInput(i) == x)
				return true;
		return false;
	}

	B32 SlpPackPass::Packer::coneTouchesObserver(const Node* n) const {
		if(observers->empty())
			return false;
		List<const Node*> cone;
		if(!dataCone(n, 64, cone))
			return true; // unbounded cone: be conservative
		for(const Node* c : cone)
			if(observers->count(c))
				return true;
		return false;
	}

	String SlpPackPass::Packer::tupleKey(const List<Node*>& lanes) {
		String k;
		k.reserve(lanes.size() * 10);
		for(Node* n : lanes) {
			k += std::to_string(n->getId());
			k.push_back(',');
		}
		return k;
	}

	Node* SlpPackPass::Packer::packTuple(const List<Node*>& lanes, Type* elemTy, U32 depth) {
		Type* vecTy = fn.types().getVec(elemTy, (U32)lanes.size());

		// splat: every lane is the same node
		B32 allSame = std::all_of(lanes.begin(), lanes.end(), [&](Node* n) { return n == lanes[0]; });
		if(allSame) {
			if(coneTouchesObserver(lanes[0]))
				return nullptr;
			profit -= 1; // one broadcast
			++st.packSplat;
			return fn.create<SplatNode>(vecTy, lanes[0]);
		}

		String key = tupleKey(lanes);
		if(auto it = memo.find(key); it != memo.end()) {
			++st.memoHits;
			return it->second;
		}

		Node* built = packTupleUncached(lanes, elemTy, vecTy, depth);
		memo.emplace(std::move(key), built);
		return built;
	}

	Node* SlpPackPass::Packer::packTupleUncached(const List<Node*>& lanes,
																							 Type* elemTy,
																							 Type* vecTy,
																							 U32 depth) {
		U32 w = (U32)lanes.size();

		// all-constant lanes
		B32 allConst =
				std::all_of(lanes.begin(), lanes.end(), [](Node* n) { return isa<ConstantNode>(n); });
		if(allConst) {
			profit -= 1;
			++st.packConst;
			return fn.create<PackNode>(vecTy, lanes);
		}

		if(depth < kMaxDepth) {
			// isomorphic binary lanes: recurse into both operand tuples
			Opcode op = lanes[0]->getOpcode();
			B32 allBin = std::all_of(lanes.begin(), lanes.end(), [&](Node* n) {
				return isa<BinaryNode>(n) && n->getOpcode() == op && n->getType() == elemTy;
			});
			if(allBin && packableBinary(op, elemTy)) {
				List<Node*> ls, rs;
				BinaryNode* b0 = cast<BinaryNode>(lanes[0]);
				ls.push_back(b0->getLHS());
				rs.push_back(b0->getRHS());
				// commutative lanes orient their operands by shape key
				U64 sl0 = shapes(b0->getLHS()), sr0 = shapes(b0->getRHS());
				for(U32 i = 1; i < w; ++i) {
					BinaryNode* b = cast<BinaryNode>(lanes[i]);
					Node* l = b->getLHS();
					Node* r = b->getRHS();
					if(b->isCommutative() && sl0 != sr0 && !shapesDisabled()) {
						U64 sl = shapes(l), sr = shapes(r);
						if(sl != sl0 && sr == sl0 && sl == sr0) {
							std::swap(l, r);
							++st.orientSwaps;
						}
					}
					ls.push_back(l);
					rs.push_back(r);
				}
				Node* lv = packTuple(ls, elemTy, depth + 1);
				Node* rv = packTuple(rs, elemTy, depth + 1);
				if(!lv || !rv)
					return nullptr;
				profit += (I32)w - 1;
				++interior;
				++st.packBinary;
				return fn.create<BinaryNode>(op, vecTy, lv, rv);
			}

			// adjacent loads: a single wide load
			B32 hardFail = false;
			if(Node* wide = packLoads(lanes, elemTy, vecTy, hardFail))
				return wide;
			if(hardFail)
				return nullptr;
		}

		// build the vector from the scalars lane by lane
		for(Node* n : lanes)
			if(coneTouchesObserver(n))
				return nullptr;
		profit -= (I32)w + 1;
		++st.packFrontier;
		return fn.create<PackNode>(vecTy, lanes);
	}

	Node* SlpPackPass::Packer::packLoads(const List<Node*>& lanes,
																			 Type* elemTy,
																			 Type* vecTy,
																			 B32& hardFail) {
		U32 w = (U32)lanes.size();
		U32 esz = elemTy->byteSize(ptrBytes);
		if(!std::all_of(lanes.begin(), lanes.end(), [&](Node* n) {
				 return isa<LoadNode>(n) && n->getType() == elemTy;
			 }))
			return nullptr;
		LoadNode* first = cast<LoadNode>(lanes[0]);
		RefinedAddr k0 = refineAddr(first->getPointer(), esz);
		if(!k0.valid())
			return nullptr;
		B32 sharedState = true;
		for(U32 i = 1; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			if(l->getControl() != first->getControl())
				return nullptr;
			sharedState &= l->getMemory() == first->getMemory();
			RefinedAddr k = refineAddr(l->getPointer(), esz);
			if(!k.valid() || !k.sameGroup(k0) || k.constant != k0.constant + (I64)(i * esz))
				return nullptr;
		}

		if(sharedState && !interWritten->count(first->getMemory())) {
			// all lanes read one pre-window state
			profit += (I32)w - 1;
			++interior;
			++st.packWideLoad;
			return fn.create<LoadNode>(
					vecTy, first->getControl(), first->getMemory(), first->getPointer());
		}

		for(U32 i = 0; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			Node* m = l->getMemory();
			if(m == memIn)
				continue;
			auto it = interWritten->find(m);
			if(it == interWritten->end())
				return nullptr; // some other state, not this window's business
			if(k0.sameGroup(*windowKey)) {
				I64 c = k0.constant + (I64)(i * esz);
				for(I64 written : it->second)
					if(written == c) {
						hardFail = true; // scalar reads a freshly stored lane
						return nullptr;
					}
			}
		}

		if(!k0.sameGroup(*windowKey)) {
			B32 distinct =
					identifiedBase(k0.base) && identifiedBase(windowKey->base) && k0.base != windowKey->base;
			if(!distinct)
				addGuard(k0, first->getPointer(), w * esz);
		}

		profit += (I32)w - 1;
		++interior;
		++st.packWideLoad;
		return fn.create<LoadNode>(vecTy, first->getControl(), memIn, first->getPointer());
	}

	StoreNode* SlpPackPass::soleChainSuccessor(StoreNode* s, List<LoadNode*>& observers) {
		StoreNode* succ = nullptr;
		for(Node* u : s->getUsers()) {
			if(u->getOpcode() == Opcode::Store && cast<StoreNode>(u)->getMemory() == s) {
				if(succ && succ != u)
					return nullptr;
				succ = cast<StoreNode>(u);
			} else if(u->getOpcode() == Opcode::Load && cast<LoadNode>(u)->getMemory() == s) {
				observers.push_back(cast<LoadNode>(u));
			} else if(usesValue(u, s)) {
				return nullptr;
			}
		}
		return succ;
	}

	U32 SlpPackPass::laneCountFor(U32 esz) { return kVecBytes / esz; }

	B32 SlpPackPass::windowAt(const Segment& seg, U32 at, WindowShape& out) {
		if(at >= seg.size())
			return false;
		const RefinedAddr& k0 = seg[at].key;
		U32 esz = k0.size;
		if(esz != 4 && esz != 8)
			return false;
		U32 w = laneCountFor(esz);
		if(at + w > seg.size())
			return false;
		Type* elemTy = seg[at].store->getValue()->getType();
		Node* ctrl = seg[at].store->getControl();
		I64 lo = k0.constant;
		for(U32 j = 0; j < w; ++j) {
			const StoreInfo& si = seg[at + j];
			if(!si.key.sameGroup(k0) || si.store->getValue()->getType() != elemTy ||
				 si.store->getControl() != ctrl)
				return false;
			lo = std::min(lo, si.key.constant);
		}
		U64 seenLanes = 0;
		for(U32 j = 0; j < w; ++j) {
			I64 d = seg[at + j].key.constant - lo;
			if(d < 0 || d % esz != 0 || (U64)d / esz >= w)
				return false;
			U64 bit = 1ull << ((U64)d / esz);
			if(seenLanes & bit)
				return false;
			seenLanes |= bit;
		}
		out.begin = at;
		out.w = w;
		out.esz = esz;
		out.elemTy = elemTy;
		out.ctrl = ctrl;
		out.lo = lo;
		out.byOff.clear();
		for(U32 j = 0; j < w; ++j)
			out.byOff.push_back(&seg[at + j]);
		std::sort(out.byOff.begin(), out.byOff.end(), [](const StoreInfo* a, const StoreInfo* b) {
			return a->key.constant < b->key.constant;
		});
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
		List<I64> written;
		for(U32 j = 0; j + 1 < count; ++j) {
			written.push_back(seg[begin + j].key.constant);
			interWritten.emplace(seg[begin + j].store, written);
			for(LoadNode* L : seg[begin + j].observers)
				obsSet.insert(L);
		}
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

	U32 SlpPackPass::Slp::tryStaticWindow(Segment& seg, U32 i, const WindowShape& w0) {
		Map<const Node*, List<I64>> interWritten;
		Set<const Node*> obsSet;
		collectInterState(seg, i, w0.w, interWritten, obsSet);
		RefinedAddr wkey = w0.byOff[0]->key;
		Packer packer(fn, aa, ptrBytes, shapes, stats);
		packer.memIn = seg[i].store->getMemory();
		packer.windowKey = &wkey;
		packer.interWritten = &interWritten;
		packer.observers = &obsSet;
		packer.profit += (I32)w0.w - 1; // the fused store itself
		Node* vec = packer.packTuple(laneValues(w0), w0.elemTy, 0);

		auto allInputsConst = [](Node* v) {
			for(U32 j = 0, e = v->getInputCount(); j < e; ++j)
				if(!isa<ConstantNode>(v->getInput(j)))
					return false;
			return true;
		};
		B32 cheapFill = vec && (vec->getOpcode() == Opcode::Splat ||
														(vec->getOpcode() == Opcode::Pack && allInputsConst(vec)));

		if(!vec || !packer.guardGroups.empty() || packer.profit < kMinProfit ||
			 !(packer.interior > 0 || cheapFill)) {
			if(!vec)
				++stats.rejectedTree;
			else
				++stats.rejectedProfit;
			return 0;
		}

		StoreNode* lastInChain = seg[i + w0.w - 1].store;
		Node* wide = fn.create<StoreNode>(
				fn.memTy(), w0.ctrl, packer.memIn, w0.byOff[0]->store->getPointer(), vec);
		lastInChain->replaceAllUsesWith(wide);
		for(U32 j = 0; j < w0.w; ++j)
			fn.removeNode(seg[i + j].store);
		++stats.packedUnguarded;
		return w0.w;
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
			++stats.windowsSeen;

			if(windowHasObs(seg, w0)) {
				++i; // guarded runs land in a later milestone
				continue;
			}

			U32 consumed = tryStaticWindow(seg, i, w0);
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
		Map<Node*, StoreInfo> cand = collectCandidates();
		if(cand.empty())
			return 0;
		List<Segment> segments = buildSegments(cand);
		U32 changed = 0;
		for(Segment& seg : segments)
			changed += processSegment(seg);
		// sweep replaced scalars and any speculative, unprofitable trees
		fn.eliminateDeadNodes();
		return changed;
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		U32 ptrBytes = target.getPointerSizeInBytes();
		AliasAnalysis aa(fn, ptrBytes);
		return Slp(fn, aa, ptrBytes, stats).run();
	}
} // namespace rat
