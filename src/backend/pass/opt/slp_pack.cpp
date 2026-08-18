#include "pass/opt/slp_pack.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "pass/opt/alias_analysis.h"
#include "target/target.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace rat {
	B32 SlpPackPass::envFlag(const C8* name) { return std::getenv(name) != nullptr; }

	B32 SlpPackPass::statsEnabled() {
		static B32 v = envFlag("RAT_SLP_STATS");
		return v;
	}

	B32 SlpPackPass::shapesDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_SHAPES");
		return v;
	}

	B32 SlpPackPass::guardsDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARDS");
		return v;
	}

	B32 SlpPackPass::guardCostDisabled() {
		static B32 v = envFlag("RAT_SLP_NO_GUARD_COST");
		return v;
	}

	B32 SlpPackPass::fpReduceEnabled() {
		static B32 v = envFlag("RAT_SLP_FP_REDUCE");
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
		case Opcode::Mul:
			return t->isInt() && t->getIntWidth() == 32; // pmulld, no packed i64 mul in sse
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
					const Node* x = b->getLHS();
					if(isa<ConstantNode>(b->getLHS()))
						x = b->getRHS();
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
			I64 sign = 1;
			if(op == Opcode::Sub)
				sign = -1;
			refineTerm(b->getRHS(), sign, out, 0);
			base = b->getLHS();
		}
		out.base = base;
		// canonicalize: merge equal vars, drop zero scales, sort by id
		std::sort(out.terms.begin(), out.terms.end(), [](const auto& a, const auto& b) {
			return a.first->getId() < b.first->getId();
		});
		List<Pair<const Node*, I64>> merged;
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
			U64 baseId = 0;
			if(ra.base)
				baseId = ra.base->getId();
			h = mix(h, baseId);
			for(const auto& t : ra.terms)
				h = mix(mix(h, t.first->getId()), (U64)t.second);
		}
		if(depth && isArithmeticOpcode(n->getOpcode())) {
			U32 e = n->getInputCount();
			if(n->isCommutative() && e == 2) {
				U64 a = shape(n->getInput(0), depth - 1);
				U64 b = shape(n->getInput(1), depth - 1);
				if(a < b)
					h = mix(h, mix(a, b));
				else
					h = mix(h, mix(b, a));
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

	// rewrite same-group wide-access pointers as anchor + byte delta so lowering
	// folds a displacement (window formation already trusts the refined deltas)
	Node* SlpPackPass::Packer::anchorPtr(Node* ptr, const RefinedAddr& k) {
		if(!addrAnchors || !k.valid())
			return ptr;
		auto [it, inserted] = addrAnchors->try_emplace(groupSig(k), ptr, k.constant);
		auto [anchor, anchorC] = it->second;
		if(inserted)
			return ptr;
		I64 delta = k.constant - anchorC;
		if(delta == 0)
			return anchor;
		Node* off = fn.create<ConstantNode>(fn.types().getInt(64), delta);
		return fn.create<BinaryNode>(Opcode::Add, ptr->getType(), anchor, off);
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

	void SlpPackPass::rewriteInput(Node* u, const Node* from, Node* to) {
		for(U32 t = 0, e = u->getInputCount(); t < e; ++t)
			if(u->getInput(t) == from)
				u->setInput(t, to);
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
		if(allSame && !isa<ConstantNode>(lanes[0]) && !coneTouchesObserver(lanes[0])) {
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
			if(allBin && packableBinary(op, elemTy) && (op != Opcode::Mul || sse41)) {
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
		B32 adjacent = true, equal = true;
		for(U32 i = 1; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			if(l->getControl() != first->getControl())
				return nullptr;
			sharedState &= l->getMemory() == first->getMemory();
			RefinedAddr k = refineAddr(l->getPointer(), esz);
			if(!k.valid() || !k.sameGroup(k0))
				return nullptr;
			adjacent &= k.constant == k0.constant + (I64)(i * esz);
			equal &= k.constant == k0.constant;
		}
		if(!adjacent && !equal)
			return nullptr;

		// one lane value broadcast: reload once from the speculation state
		auto reloadSplat = [&](Node* mem) -> Node* {
			profit += (I32)w - 2; // w scalar loads become one load and one broadcast
			++st.packSplat;
			Node* ld =
					fn.create<LoadNode>(elemTy, first->getControl(), mem, anchorPtr(first->getPointer(), k0));
			Node* sp = fn.create<SplatNode>(vecTy, ld);
			splatLoads.push_back({sp, cast<LoadNode>(ld)});
			return sp;
		};

		if(sharedState && !interWritten->count(first->getMemory())) {
			// all lanes read one pre-window state
			if(equal)
				return reloadSplat(first->getMemory());
			profit += (I32)w - 1;
			++interior;
			++st.packWideLoad;
			return fn.create<LoadNode>(
					vecTy, first->getControl(), first->getMemory(), anchorPtr(first->getPointer(), k0));
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
				I64 laneOff = 0;
				if(adjacent)
					laneOff = (I64)(i * esz);
				I64 c = k0.constant + laneOff;
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
			if(!distinct) {
				U32 guardBytes = w * esz;
				if(equal)
					guardBytes = esz;
				addGuard(k0, first->getPointer(), guardBytes);
			}
		}

		if(equal)
			return reloadSplat(memIn);
		profit += (I32)w - 1;
		++interior;
		++st.packWideLoad;
		return fn.create<LoadNode>(
				vecTy, first->getControl(), memIn, anchorPtr(first->getPointer(), k0));
	}

	// splat reloads at consecutive addresses fold into one wide load
	// each broadcast then picks its lane in-register instead of touching memory
	void SlpPackPass::Packer::coalesceSplats() {
		struct Cand {
			String sig; // address group + state + control
			I64 c;
			Node* splat;
			LoadNode* load;
		};
		List<Cand> cs;
		for(auto& [splat, load] : splatLoads) {
			U32 esz = load->getType()->byteSize(ptrBytes);
			RefinedAddr k = refineAddr(load->getPointer(), esz);
			if((esz != 4 && esz != 8) || !k.valid())
				continue;
			String sig = groupSig(k) + '@' + std::to_string(load->getMemory()->getId()) + '@' +
									 std::to_string(load->getControl()->getId());
			cs.push_back({std::move(sig), k.constant, splat, load});
		}
		std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) {
			if(a.sig != b.sig)
				return a.sig < b.sig;
			return a.c < b.c;
		});
		for(U32 i = 0; i < cs.size();) {
			U32 esz = cs[i].load->getType()->byteSize(ptrBytes);
			U32 w = kVecBytes / esz;
			Type* vecTy = fn.types().getVec(cs[i].load->getType(), w);
			B32 run = i + w <= (U32)cs.size() && cs[i].splat->getType() == vecTy;
			for(U32 j = 1; run && j < w; ++j)
				run = cs[i + j].sig == cs[i].sig && cs[i + j].c == cs[i].c + (I64)(j * esz) &&
							cs[i + j].splat->getType() == vecTy;
			if(!run) {
				++i;
				continue;
			}
			Node* wide = fn.create<LoadNode>(
					vecTy, cs[i].load->getControl(), cs[i].load->getMemory(), cs[i].load->getPointer());
			for(U32 j = 0; j < w; ++j) {
				U8 sel;
				if(esz == 4)
					sel = (U8)(j * 0x55);
				else if(j)
					sel = (U8)0xee;
				else
					sel = (U8)0x44;
				cs[i + j].splat->replaceAllUsesWith(fn.create<ShuffleNode>(vecTy, wide, sel));
				++st.splatGrouped;
			}
			i += w;
		}
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
			Node* m = l->getMemory();
			while(StoreNode* s = dyn_cast<StoreNode>(m)) {
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

	void SlpPackPass::Slp::normalizeStoreChains() {
		auto storeKey = [&](StoreNode* s, RefinedAddr& out) -> B32 {
			U32 sz = aa.getAccessSize(s);
			if(!sz)
				return false;
			out = refineAddr(s->getPointer(), sz);
			return out.valid();
		};
		B32 progress = true;
		for(U32 round = 0; progress && round < 8; ++round) {
			progress = false;
			for(Node* n : fn) {
				StoreNode* s = dyn_cast<StoreNode>(n);
				if(!s)
					continue;
				RefinedAddr ks;
				if(!storeKey(s, ks))
					continue;
				String sig = groupSig(ks);
				U32 hops = 0;
				while(hops++ < 16) {
					StoreNode* p = dyn_cast<StoreNode>(s->getMemory());
					if(!p || p->getControl() != s->getControl())
						break;
					RefinedAddr kp;
					if(!storeKey(p, kp))
						break;
					String psig = groupSig(kp);
					if(psig == sig || sig >= psig)
						break; // same group, or already canonically ordered
					if(!provablyDisjoint(aa, s->getPointer(), ks, ks.size, p->getPointer(), kp, kp.size))
						break;
					// p's output must have no observer besides s
					B32 sole = true;
					for(Node* u : p->getUsers())
						if(u != s && usesValue(u, p)) {
							sole = false;
							break;
						}
					if(!sole)
						break;
					// swap: ... -> pp -> p -> s  ==>  ... -> pp -> s -> p
					List<Node*> sUsers;
					for(Node* u : s->getUsers())
						sUsers.push_back(u);
					s->setInput(1, p->getMemory());
					p->setInput(1, s);
					for(Node* u : sUsers)
						if(u != p)
							rewriteInput(u, s, p);
					progress = true;
				}
			}
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
		Packer packer(fn, aa, ptrBytes, sse41, shapes, stats);
		packer.memIn = seg[i].store->getMemory();
		packer.windowKey = &wkey;
		packer.interWritten = &interWritten;
		packer.observers = &obsSet;
		packer.addrAnchors = &addrAnchors;
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
		Node* wide = fn.create<StoreNode>(fn.memTy(),
																			w0.ctrl,
																			packer.memIn,
																			packer.anchorPtr(w0.byOff[0]->store->getPointer(), wkey),
																			vec);
		lastInChain->replaceAllUsesWith(wide);
		for(U32 j = 0; j < w0.w; ++j)
			fn.removeNode(seg[i + j].store);
		packer.coalesceSplats();
		++stats.packedUnguarded;
		return w0.w;
	}

	U32 SlpPackPass::Slp::tryGuardedRun(Segment& seg, U32 i, const WindowShape& w0) {
		List<WindowShape> run = {w0};
		while(run.size() < kMaxRunWindows) {
			WindowShape wn;
			if(!windowAt(seg, i + (U32)run.size() * w0.w, wn))
				break;
			if(!wn.byOff[0]->key.sameGroup(w0.byOff[0]->key) ||
				 wn.lo != w0.lo + (I64)(run.size() * kVecBytes) || wn.ctrl != w0.ctrl || wn.esz != w0.esz)
				break;
			run.push_back(std::move(wn));
		}
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		stats.windowsSeen += n - 1; // run extensions
		Node* memIn = seg[i].store->getMemory();
		Node* wPtr = w0.byOff[0]->store->getPointer();
		RefinedAddr wkey = w0.byOff[0]->key;
		Map<const Node*, List<I64>> interWritten;
		Set<const Node*> obsSet;
		collectInterState(seg, i, total, interWritten, obsSet);

		Packer packer(fn, aa, ptrBytes, sse41, shapes, stats);
		packer.memIn = memIn;
		packer.windowKey = &wkey;
		packer.interWritten = &interWritten;
		packer.observers = &obsSet;
		packer.addrAnchors = &addrAnchors;
		List<Node*> vecs;
		B32 treeOk = true;
		for(U32 k = 0; k < n && treeOk; ++k) {
			packer.profit += (I32)w0.w - 1; // each fused store
			if(Node* v = packer.packTuple(laneValues(run[k]), w0.elemTy, 0))
				vecs.push_back(v);
			else
				treeOk = false;
		}

		I32 guardCost = 0;
		if(!guardCostDisabled())
			guardCost = (I32)packer.guardGroups.size() * kGuardCheckCost + kGuardBranchCost;
		B32 accept = treeOk && packer.profit - guardCost >= (I32)(kMinProfit * n) &&
								 packer.interior >= n && packer.guardGroups.size() <= kMaxGuards &&
								 !packer.coneTouchesObserver(wPtr);
		for(const auto& g : packer.guardGroups)
			if(accept)
				accept = !packer.coneTouchesObserver(g.ptr);
		if(accept)
			accept = observersConfined(seg, i, total, obsSet);
		if(!accept) {
			if(!treeOk)
				++stats.rejectedTree;
			else
				++stats.rejectedGuarded;
			return 0;
		}

		commitGuardedRun(seg, i, run, vecs, packer, interWritten);
		packer.coalesceSplats();
		stats.packedGuarded += n;
		++stats.guardedRuns;
		stats.guardPairs += (U32)packer.guardGroups.size();
		return total;
	}

	B32 SlpPackPass::Slp::observersConfined(const Segment& seg,
																					U32 i,
																					U32 total,
																					const Set<const Node*>& obsSet) const {
		Set<const Node*> runStores;
		for(U32 j = 0; j < total; ++j)
			runStores.insert(seg[i + j].store);
		for(const Node* obs : obsSet) {
			List<const Node*> work = {obs};
			Set<const Node*> seen;
			while(!work.empty()) {
				const Node* c = work.back();
				work.pop_back();
				if(!seen.insert(c).second)
					continue;
				if(seen.size() > 128)
					return false;
				for(Node* u : c->getUsers()) {
					if(runStores.count(u))
						continue;
					if(isArithmeticOpcode(u->getOpcode()))
						work.push_back(u);
					else
						return false;
				}
			}
		}
		return true;
	}

	void SlpPackPass::Slp::commitGuardedRun(Segment& seg,
																					U32 i,
																					const List<WindowShape>& run,
																					const List<Node*>& vecs,
																					Packer& packer,
																					const Map<const Node*, List<I64>>& interWritten) {
		const WindowShape& w0 = run[0];
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		Node* memIn = seg[i].store->getMemory();
		Node* wPtr = w0.byOff[0]->store->getPointer();
		StoreNode* lastInChain = seg[i + total - 1].store;
		Set<const Node*> runStores;
		for(U32 j = 0; j < total; ++j)
			runStores.insert(seg[i + j].store);
		Type* i64 = fn.types().getInt(64);
		Type* boolTy = fn.boolTy();
		Type* ctrlTy = fn.ctrlTy();
		I64 runBytes = (I64)(n * kVecBytes);
		Type* iffTy = fn.types().getTuple({ctrlTy, ctrlTy});
		Node* wEnd = fn.create<BinaryNode>(
				Opcode::Add, wPtr->getType(), wPtr, fn.create<ConstantNode>(i64, runBytes));
		auto branch = [&](Node* c, Node* pred) { return fn.create<IfNode>(iffTy, c, pred); };
		auto proj = [&](Node* of, U32 index, const C8* label) {
			return fn.create<ProjNode>(ctrlTy, of, index, label);
		};

		// short-circuit cascade
		Node* iff = nullptr;
		Node* ctrl = w0.ctrl;
		List<Node*> fails;
		for(const auto& g : packer.guardGroups) {
			Node* gEnd = fn.create<BinaryNode>(
					Opcode::Add, g.ptr->getType(), g.ptr, fn.create<ConstantNode>(i64, g.maxC - g.minC));
			Node* bIf = branch(ctrl, fn.create<CompareNode>(Opcode::Ule, boolTy, gEnd, wPtr));
			Node* bT = proj(bIf, IfNode::thenProjIndex(), "slp.then");
			Node* bF = proj(bIf, IfNode::elseProjIndex(), "slp.chk");
			Node* aIf = branch(bF, fn.create<CompareNode>(Opcode::Ule, boolTy, wEnd, g.ptr));
			Node* aT = proj(aIf, IfNode::thenProjIndex(), "slp.then");
			fails.push_back(proj(aIf, IfNode::elseProjIndex(), "slp.else"));
			if(!iff)
				iff = bIf;
			ctrl = fn.create<RegionNode>(ctrlTy, List<Node*>{bT, aT});
		}
		if(!iff) { // observers but no may-alias load groups
			iff = branch(w0.ctrl, fn.create<ConstantNode>(boolTy, 1));
			ctrl = proj(iff, IfNode::thenProjIndex(), "slp.then");
			fails.push_back(proj(iff, IfNode::elseProjIndex(), "slp.else"));
		}
		Node* thenP = ctrl;
		Node* elseP = nullptr;
		if(fails.size() == 1)
			elseP = fails[0];
		else
			elseP = fn.create<RegionNode>(ctrlTy, fails);
		Node* prevMem = memIn;
		Set<const Node*> wideStores;
		for(U32 k = 0; k < n; ++k) {
			Node* wide = fn.create<StoreNode>(
					fn.memTy(),
					thenP,
					prevMem,
					packer.anchorPtr(run[k].byOff[0]->store->getPointer(), run[k].byOff[0]->key),
					vecs[k]);
			wideStores.insert(wide);
			prevMem = wide;
		}
		Node* region = fn.create<RegionNode>(ctrlTy, List<Node*>{thenP, elseP});

		// post-run consumers of the final state read the merge
		List<Node*> lastUsers;
		for(Node* u : lastInChain->getUsers())
			lastUsers.push_back(u);
		PhiNode* phi = fn.create<PhiNode>(fn.memTy(), List<Node*>{region, prevMem, lastInChain});
		for(Node* u : lastUsers)
			rewriteInput(u, lastInChain, phi);

		// scalar chain moves to the else arm
		for(U32 j = 0; j < total; ++j)
			seg[i + j].store->setInput(0, elseP);

		// pre-run states: everything the incoming state derives from
		Set<const Node*> preSet;
		{
			Node* m = memIn;
			U32 hop = 0;
			while(m && hop++ < 4096) {
				preSet.insert(m);
				if(m->getOpcode() == Opcode::Store)
					m = cast<StoreNode>(m)->getMemory();
				else if(ProjNode* p = dyn_cast<ProjNode>(m)) {
					Node* prod = p->getProducer();
					if(prod->getOpcode() == Opcode::Call)
						m = cast<CallNode>(prod)->getMemory();
					else
						break; // start (or foreign): done
				} else {
					break; // phi or other merge: done
				}
			}
		}

		// value cone ends entirely in the replaced scalar stores
		auto feedsOnlyRun = [&](const Node* n) -> B32 {
			List<const Node*> work = {n};
			Set<const Node*> seen;
			while(!work.empty()) {
				const Node* c = work.back();
				work.pop_back();
				if(!seen.insert(c).second)
					continue;
				if(seen.size() > 64)
					return false;
				for(Node* u : c->getUsers()) {
					if(runStores.count(u))
						continue;
					if(isArithmeticOpcode(u->getOpcode()))
						work.push_back(u);
					else
						return false;
				}
			}
			return true;
		};

		// re-route the rest of the block
		List<Node*> ctrlUsers;
		for(Node* u : w0.ctrl->getUsers())
			ctrlUsers.push_back(u);
		for(Node* u : ctrlUsers) {
			if(u == iff || wideStores.count(u) || runStores.count(u))
				continue;
			Node* dest = nullptr; // null: keep the pre-branch anchor
			if(isControlNode(u)) {
				dest = region; // terminator or successor-region edge
			} else {
				Node* m = nullptr;
				if(LoadNode* ld = dyn_cast<LoadNode>(u))
					m = ld->getMemory();
				else if(StoreNode* st = dyn_cast<StoreNode>(u))
					m = st->getMemory();
				else if(CallNode* cl = dyn_cast<CallNode>(u))
					m = cl->getMemory();
				if(m && interWritten.count(m))
					dest = elseP;
				else if(m && !preSet.count(m))
					dest = region; // post-run
				else if(m && isa<LoadNode>(u) && feedsOnlyRun(u))
					dest = elseP; // pre-run load only the scalar arm reads
			}
			if(dest)
				rewriteInput(u, w0.ctrl, dest);
		}
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
			// scalar fallback arm of an earlier guard
			auto isElseProj = [](const Node* c) {
				const ProjNode* p = dyn_cast<ProjNode>(c);
				return p && p->getLabel() && String(p->getLabel()) == "slp.else";
			};
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

	U32 SlpPackPass::Slp::packReduction(BinaryNode* root) {
		Type* t = root->getType();
		U32 esz = t->byteSize(ptrBytes);
		U32 w = laneCountFor(esz);
		Opcode addOp = root->getOpcode();

		// flatten through single-use interior adds, right operand first
		List<Node*> terms;
		List<Node*> work = {root};
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			BinaryNode* b = dyn_cast<BinaryNode>(n);
			B32 interior = b && b->getOpcode() == addOp && b->getType() == t &&
										 (n == root || n->getUsers().size() == 1);
			if(interior && terms.size() + work.size() < 64) {
				work.push_back(b->getLHS());
				work.push_back(b->getRHS());
			} else {
				terms.push_back(n);
			}
		}
		std::reverse(terms.begin(), terms.end()); // back to source order
		U32 n = (U32)terms.size();
		if(n < 2 * w || n % w != 0)
			return 0;
		++stats.windowsSeen;

		// canonical term order: sort by the first leaf load's refined address so
		// grouping is robust against source-level reassociation
		auto leafKey = [&](Node* term, RefinedAddr& out) -> B32 {
			List<const Node*> cone;
			dataCone(term, 64, cone);
			for(const Node* c : cone)
				if(const LoadNode* l = dyn_cast<LoadNode>(c)) {
					out = refineAddr(const_cast<LoadNode*>(l)->getPointer(), esz);
					return out.valid();
				}
			return false;
		};
		List<Pair<Pair<String, I64>, Node*>> keyed;
		for(Node* term : terms) {
			RefinedAddr k;
			if(!leafKey(term, k))
				break;
			keyed.push_back({{groupSig(k), k.constant}, term});
		}
		if(keyed.size() == terms.size()) {
			std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
				return a.first < b.first;
			});
			for(U32 i = 0; i < n; ++i)
				terms[i] = keyed[i].second;
		}

		// anchor the packer on the first leaf load's state
		Node* memIn = nullptr;
		RefinedAddr wkey;
		{
			List<const Node*> cone;
			dataCone(root, 128, cone);
			for(const Node* c : cone)
				if(const LoadNode* l = dyn_cast<LoadNode>(c)) {
					LoadNode* ld = const_cast<LoadNode*>(l);
					memIn = ld->getMemory();
					wkey = refineAddr(ld->getPointer(), esz);
					break;
				}
		}

		Map<const Node*, List<I64>> emptyIw;
		Set<const Node*> emptyObs;
		Packer packer(fn, aa, ptrBytes, sse41, shapes, stats);
		packer.memIn = memIn;
		packer.windowKey = &wkey;
		packer.interWritten = &emptyIw;
		packer.observers = &emptyObs;
		packer.addrAnchors = &addrAnchors;

		U32 k = n / w;
		List<Node*> vecs;
		for(U32 g = 0; g < k; ++g) {
			List<Node*> lanes(terms.begin() + g * w, terms.begin() + (g + 1) * w);
			Node* v = packer.packTuple(lanes, t, 0);
			if(!v) {
				++stats.rejectedTree;
				return 0;
			}
			vecs.push_back(v);
		}
		if(!packer.guardGroups.empty()) {
			++stats.rejectedGuarded;
			return 0;
		}

		I32 hsumOps = 3;
		if(w == 4)
			hsumOps = 5;
		packer.profit += (I32)n - 1;					 // scalar add chain removed
		packer.profit -= (I32)k - 1 + hsumOps; // vector combine + horizontal finish
		if(packer.profit < kMinProfit || packer.interior == 0) {
			++stats.rejectedProfit;
			return 0;
		}

		Type* vecTy = fn.types().getVec(t, w);
		Node* acc = vecs[0];
		for(U32 g = 1; g < k; ++g)
			acc = fn.create<BinaryNode>(addOp, vecTy, acc, vecs[g]);
		// log2 shuffle+add finish, result in every lane
		Node* s1 = fn.create<ShuffleNode>(vecTy, acc, (U8)0x4e); // swap 64-bit halves
		acc = fn.create<BinaryNode>(addOp, vecTy, acc, s1);
		if(w == 4) {
			Node* s2 = fn.create<ShuffleNode>(vecTy, acc, (U8)0xb1); // swap 32-bit pairs
			acc = fn.create<BinaryNode>(addOp, vecTy, acc, s2);
		}
		Node* res = fn.create<ExtractNode>(t, acc, 0);
		root->replaceAllUsesWith(res);
		++stats.packedReduction;
		return 1;
	}

	U32 SlpPackPass::Slp::packReductions() {
		List<BinaryNode*> roots;
		for(Node* n : fn) {
			BinaryNode* b = dyn_cast<BinaryNode>(n);
			if(!b)
				continue;
			Opcode op = b->getOpcode();
			if(op != Opcode::Add && !(op == Opcode::FAdd && fpReduceEnabled()))
				continue;
			Type* t = b->getType();
			if(!t || !packableElem(t) || t->isInt() != (op == Opcode::Add))
				continue;
			B32 isRoot = true;
			for(Node* u : b->getUsers())
				if(u->getOpcode() == op && u->getType() == t)
					isRoot = false;
			if(isRoot)
				roots.push_back(b);
		}
		U32 changed = 0;
		for(BinaryNode* root : roots)
			changed += packReduction(root);
		return changed;
	}

	U32 SlpPackPass::Slp::run() {
		normalizeLoadEdges();
		normalizeStoreChains();
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

	B32 SlpPackPass::run(Module& module, const TargetInfo& target) {
		stats = SlpStats{};
		B32 changed = FunctionPass::run(module, target);
		if(statsEnabled()) {
			const SlpStats& s = stats;
			std::cerr << "slp[" << module.getName() << "]: windows " << s.windowsSeen << " packed "
								<< (s.packedUnguarded + s.packedGuarded) << " (static " << s.packedUnguarded
								<< ", guarded " << s.packedGuarded << " in " << s.guardedRuns << " runs, "
								<< s.guardPairs << " checks)"
								<< " reductions " << s.packedReduction << " rejected "
								<< (s.rejectedTree + s.rejectedProfit + s.rejectedGuarded) << " (tree "
								<< s.rejectedTree << ", profit " << s.rejectedProfit << ", guard "
								<< s.rejectedGuarded << ")\n"
								<< "slp[" << module.getName() << "]: nodes: wload " << s.packWideLoad << " vbin "
								<< s.packBinary << " splat " << s.packSplat << " (grouped " << s.splatGrouped
								<< ") const " << s.packConst << " frontier " << s.packFrontier << " | orient-swaps "
								<< s.orientSwaps << " memo-hits " << s.memoHits << "\n";
		}
		return changed;
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		U32 ptrBytes = target.getPointerSizeInBytes();
		AliasAnalysis aa(fn, ptrBytes);
		return Slp(fn, aa, ptrBytes, target.hasSse41(), stats).run();
	}
} // namespace rat
