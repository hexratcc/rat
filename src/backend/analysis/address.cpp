#include "analysis/address.h"

#include "analysis/alias_analysis.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	namespace detail {
		// add scale * n to constant and terms. narrow: n is an i32 under a sext, so an add or sub
		// splits off only a constant and a shift stays below 31
		void refineTerm(const Node* n, I64 scale, B32 narrow, I64& constant, Terms& terms, U32 depth) {
			const BinaryNode* b = dyn_cast<BinaryNode>(n);
			const ConstantNode* k = nullptr;
			if(b)
				k = dyn_cast<ConstantNode>(b->getRHS()); // fold puts a lone constant on the right
			Opcode op = n->getOpcode();
			const Type* in = nullptr;
			if(op == Opcode::SExt)
				in = n->getInput(0)->getType();
			B32 wide = n->getType()->isInt() && n->getType()->getIntWidth() == 64;
			B32 split = b && depth < kMaxRefineDepth && (narrow || wide);
			if(scale == 0) // a cancelled var stays in the key, so a group's pointers share their vars
				terms.push_back({n, 0});
			else if(const ConstantNode* c = dyn_cast<ConstantNode>(n))
				constant += scale * c->getValue();
			else if(in && !narrow && in->isInt() && in->getIntWidth() == 32)
				refineTerm(n->getInput(0), scale, true, constant, terms, depth + 1);
			else if(split && (op == Opcode::Add || op == Opcode::Sub) && (k || !narrow)) {
				I64 rhsScale = scale;
				if(op == Opcode::Sub)
					rhsScale = -scale;
				refineTerm(b->getLHS(), scale, narrow, constant, terms, depth + 1);
				refineTerm(b->getRHS(), rhsScale, narrow, constant, terms, depth + 1);
			} else if(split && op == Opcode::Mul && k)
				refineTerm(b->getLHS(), scale * k->getValue(), narrow, constant, terms, depth + 1);
			else if(split && op == Opcode::Shl && k && k->getValue() >= 0 &&
							k->getValue() < 63 - 32 * narrow)
				refineTerm(b->getLHS(), scale << k->getValue(), narrow, constant, terms, depth + 1);
			else
				terms.push_back({n, scale});
		}

		void mergeTerms(Terms& terms) {
			std::sort(terms.begin(), terms.end());
			U32 n = 0;
			for(U32 i = 0; i < terms.size(); ++i) {
				if(n && terms[n - 1].first == terms[i].first)
					terms[n - 1].second += terms[i].second;
				else
					terms[n++] = terms[i];
			}
			terms.resize(n);
		}
	} // namespace detail

	AddrCache::AddrCache(U64 restrictParams, U32 ids)
	: restrictParams(restrictParams),
		slots(ids) {
		addrs.reserve(16); // most functions access few addresses
	}

	Addr AddrCache::get(Node* ptr) {
		U32 id = ptr->getId();
		if(id < slots.size() && slots[id])
			return addrs[slots[id] - 1];
		I64 offset = 0;
		key.clear();
		Node* base = ptr;
		for(U32 hops = 0; hops < detail::kMaxPtrHops; ++hops) {
			BinaryNode* b = dyn_cast<BinaryNode>(base);
			if(!b || (b->getOpcode() != Opcode::Add && b->getOpcode() != Opcode::Sub) ||
				 !b->getLHS()->getType()->isPtr())
				break;
			I64 sign = 1;
			if(b->getOpcode() == Opcode::Sub)
				sign = -1;
			detail::refineTerm(b->getRHS(), sign, false, offset, key, 0);
			base = b->getLHS();
		}
		detail::mergeTerms(key);
		B32 terms = !key.empty();
		key.push_back({base, 0});
		Addr* g = nullptr;
		if(terms)
			g = &groups.try_emplace(key).first->second;
		else
			g = &bases.try_emplace(base->getId()).first->second;
		if(!g->group) {
			*g = classify(key);
			g->group = (U32)(groups.size() + bases.size());
		}
		Addr a = *g;
		a.offset = offset;
		if(id >= slots.size())
			slots.resize(2 * id + 2);
		addrs.push_back(a);
		slots[id] = (U32)addrs.size();
		return a;
	}

	// kind and object id of a group's base
	// unknown when a term may carry a restrict parameter, as a + (d - a) is based on d
	Addr AddrCache::classify(const detail::Terms& terms) {
		Addr a;
		const Node* base = terms.back().first;
		if(restrictParams)
			for(U32 i = 0; i + 1 < terms.size(); ++i) {
				const Node* var = terms[i].first;
				if(AliasAnalysis::mayCarryRestrict({var}, restrictParams, AliasAnalysis::kMaxCarryCone))
					return a;
			}
		I32 index = AliasAnalysis::paramIndex(base);
		if(const GlobalNode* g = dyn_cast<GlobalNode>(base)) {
			U32 number = symbols.try_emplace(g->getSymbol(), (U32)symbols.size()).first->second;
			a.kind = BaseKind::Object;
			a.object = detail::kObjectTags * number + detail::kGlobalTag;
		} else if(AliasAnalysis::isIdentified(base)) {
			a.kind = BaseKind::Object;
			a.object = detail::kObjectTags * base->getId() + detail::kAllocTag;
		} else if(AliasAnalysis::isRestrictParam(index, restrictParams)) {
			a.kind = BaseKind::Restrict;
			a.object = detail::kObjectTags * (U32)index + detail::kRestrictTag;
		} else if(index >= 0) {
			a.kind = BaseKind::Param;
		}
		return a;
	}

	B32 disjointBases(const Addr& a, const Addr& b) {
		if(a.object == b.object)
			return false; // one object, or two bases without one
		if(a.object && b.object)
			return true;
		return (a.kind == BaseKind::Restrict && b.kind == BaseKind::Param) ||
					 (a.kind == BaseKind::Param && b.kind == BaseKind::Restrict);
	}

	B32 disjoint(const Addr& a, U32 sza, const Addr& b, U32 szb) {
		if(a.group != b.group)
			return disjointBases(a, b);
		return a.group && (a.offset + (I64)sza <= b.offset || b.offset + (I64)szb <= a.offset);
	}
} // namespace rat
