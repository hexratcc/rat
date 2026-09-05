#include "analysis/address.h"

#include "analysis/alias_analysis.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

#include <algorithm>

namespace rat {
	namespace {
		// extract a compile-time constant value; false if n is not a constant
		B32 constValue(const Node* n, I64& v) {
			const ConstantNode* c = dyn_cast<ConstantNode>(n);
			if(c)
				v = c->getValue();
			return c != nullptr;
		}

		B32 isI64(const Node* n) {
			return n->getType() && n->getType()->isInt() && n->getType()->getIntWidth() == 64;
		}

		B32 termLess(const Pair<const Node*, I64>& a, const Pair<const Node*, I64>& b) {
			return a.first->getId() < b.first->getId();
		}

		void refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth) {
			if(scale == 0)
				return;
			if(const ConstantNode* c = dyn_cast<ConstantNode>(n)) {
				out.constant += scale * c->getValue();
				return;
			}
			if(depth < 12) {
				if(const BinaryNode* b = dyn_cast<BinaryNode>(n)) {
					Opcode op = b->getOpcode();
					I64 rv;
					B32 rc = constValue(b->getRHS(), rv);
					if(op == Opcode::Add && rc) {
						refineTerm32(b->getLHS(), scale, out, depth + 1);
						out.constant += scale * rv;
						return;
					}
					if(op == Opcode::Sub && rc) {
						refineTerm32(b->getLHS(), scale, out, depth + 1);
						out.constant -= scale * rv;
						return;
					}
					if(op == Opcode::Mul && rc) {
						refineTerm32(b->getLHS(), scale * rv, out, depth + 1);
						return;
					}
					if(op == Opcode::Shl && rc && rv >= 0 && rv < 31) {
						refineTerm32(b->getLHS(), scale << rv, out, depth + 1);
						return;
					}
				}
			}
			out.terms.push_back({n, scale});
		}

		void refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth) {
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
					if(op == Opcode::Mul && (constValue(b->getRHS(), k) || constValue(b->getLHS(), k))) {
						// the non-constant operand carries the term; the constant folds into the scale
						const Node* x = b->getLHS();
						if(isa<ConstantNode>(b->getLHS()))
							x = b->getRHS();
						refineTerm(x, scale * k, out, depth + 1);
						return;
					}
					if(op == Opcode::Shl && constValue(b->getRHS(), k) && k >= 0 && k < 63) {
						refineTerm(b->getLHS(), scale << k, out, depth + 1);
						return;
					}
				}
			}
			out.terms.push_back({n, scale});
		}
	} // namespace

	B32 RefinedAddr::valid() const { return base != nullptr && size != 0; }

	B32 RefinedAddr::sameGroup(const RefinedAddr& o) const {
		return base == o.base && size == o.size && terms == o.terms;
	}

	// merge equal vars, drop zero scales, sort by node id
	void canonicalizeTerms(List<Pair<const Node*, I64>>& terms) {
		std::sort(terms.begin(), terms.end(), termLess);
		List<Pair<const Node*, I64>> merged;
		for(const auto& t : terms) {
			if(!merged.empty() && merged.back().first == t.first)
				merged.back().second += t.second;
			else
				merged.push_back(t);
		}
		terms.clear();
		for(const auto& t : merged)
			if(t.second != 0)
				terms.push_back(t);
	}

	RefinedAddr refineAddr(Node* addr, U32 accessBytes) {
		RefinedAddr out;
		out.size = accessBytes;
		Node* base = addr;
		U32 hops = 0;
		while(BinaryNode* b = dyn_cast<BinaryNode>(base)) {
			Opcode op = b->getOpcode();
			B32 addSub = op == Opcode::Add || op == Opcode::Sub;
			B32 ptrWalk = b->getType()->isPtr() && b->getLHS()->getType()->isPtr();
			if(++hops > 32 || !addSub || !ptrWalk)
				break;
			I64 sign = 1;
			if(op == Opcode::Sub)
				sign = -1;
			refineTerm(b->getRHS(), sign, out, 0);
			base = b->getLHS();
		}
		out.base = base;
		canonicalizeTerms(out.terms);
		return out;
	}

	B32 provablyDisjoint(const RefinedAddr& ka, U32 sza, const RefinedAddr& kb, U32 szb) {
		if(!ka.valid() || !kb.valid())
			return false;
		if(ka.base != kb.base)
			return AliasAnalysis::distinctObjects(ka.base, kb.base);
		if(ka.terms != kb.terms)
			return false;
		return ka.constant + (I64)sza <= kb.constant || kb.constant + (I64)szb <= ka.constant;
	}

	B32 provablyDisjoint(const AliasAnalysis& aa,
											 Node* pa,
											 const RefinedAddr& ka,
											 U32 sza,
											 Node* pb,
											 const RefinedAddr& kb,
											 U32 szb) {
		if(provablyDisjoint(ka, sza, kb, szb))
			return true;
		return aa.alias(pa, sza, pb, szb) == AliasResult::NoAlias;
	}
} // namespace rat
