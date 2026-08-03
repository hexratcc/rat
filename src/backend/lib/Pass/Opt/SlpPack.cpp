#include "Pass/Opt/SlpPack.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Pass/Opt/AliasAnalysis.h"
#include "Target/Target.h"

#include <algorithm>

namespace rat {
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

	B32 SlpPackPass::usesValue(const Node* u, const Node* x) {
		for(U32 i = 0, e = u->getInputCount(); i < e; ++i)
			if(u->getInput(i) == x)
				return true;
		return false;
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

	U32 SlpPackPass::Slp::processSegment(Segment& seg) {
		U32 i = 0;
		while(i < seg.size()) {
			WindowShape w0;
			if(!windowAt(seg, i, w0)) {
				++i;
				continue;
			}
			++i;
		}
		return 0;
	}

	U32 SlpPackPass::Slp::run() {
		Map<Node*, StoreInfo> cand = collectCandidates();
		if(cand.empty())
			return 0;
		List<Segment> segments = buildSegments(cand);
		U32 changed = 0;
		for(Segment& seg : segments)
			changed += processSegment(seg);
		return changed;
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		U32 ptrBytes = target.getPointerSizeInBytes();
		AliasAnalysis aa(fn, ptrBytes);
		return Slp(fn, aa, ptrBytes).run();
	}
} // namespace rat
