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
			const BinaryNode* bin = dyn_cast<BinaryNode>(n);
			const ConstantNode* rhsConst = nullptr;
			if(bin)
				rhsConst = dyn_cast<ConstantNode>(bin->getRHS()); // fold puts a lone constant on the right
			Opcode op = n->getOpcode();
			B32 sextOfI32 = op == Opcode::SExt && n->getInput(0)->getType()->isInt() &&
											n->getInput(0)->getType()->getIntWidth() == 32;
			B32 is64 = n->getType()->isInt() && n->getType()->getIntWidth() == 64;
			B32 canSplit = bin && depth < kMaxRefineDepth && (narrow || is64);
			if(scale == 0) // a cancelled var stays in the key, so a group's pointers share their vars
				terms.push_back({n, 0});
			else if(const ConstantNode* c = dyn_cast<ConstantNode>(n))
				constant += scale * c->getValue();
			else if(sextOfI32 && !narrow)
				refineTerm(n->getInput(0), scale, true, constant, terms, depth + 1);
			else if(canSplit && (op == Opcode::Add || op == Opcode::Sub) && (rhsConst || !narrow)) {
				I64 rhsScale = scale;
				if(op == Opcode::Sub)
					rhsScale = -scale;
				refineTerm(bin->getLHS(), scale, narrow, constant, terms, depth + 1);
				refineTerm(bin->getRHS(), rhsScale, narrow, constant, terms, depth + 1);
			} else if(canSplit && op == Opcode::Mul && rhsConst)
				refineTerm(bin->getLHS(), scale * rhsConst->getValue(), narrow, constant, terms, depth + 1);
			else if(canSplit && op == Opcode::Shl && rhsConst && rhsConst->getValue() >= 0 &&
							rhsConst->getValue() < 63 - 32 * narrow)
				refineTerm(
						bin->getLHS(), scale << rhsConst->getValue(), narrow, constant, terms, depth + 1);
			else
				terms.push_back({n, scale});
		}

		void mergeTerms(Terms& terms) {
			std::sort(terms.begin(), terms.end());
			U32 kept = 0;
			for(U32 i = 0; i < terms.size(); ++i) {
				if(kept && terms[kept - 1].first == terms[i].first)
					terms[kept - 1].second += terms[i].second;
				else
					terms[kept++] = terms[i];
			}
			terms.resize(kept);
		}
	} // namespace detail

	AddrCache::AddrCache(U64 restrictParams, U32 idBound)
	: restrictParams(restrictParams),
		slotOf(idBound) {
		cached.reserve(16);
	}

	Addr AddrCache::get(Node* ptr) {
		U32 id = ptr->getId();
		if(id < slotOf.size() && slotOf[id])
			return cached[slotOf[id] - 1];
		Addr a = decompose(ptr);
		if(id >= slotOf.size())
			slotOf.resize(2 * id + 2);
		cached.push_back(a);
		slotOf[id] = (U32)cached.size();
		return a;
	}

	Addr AddrCache::decompose(Node* ptr) {
		I64 offset = 0;
		key.clear();
		Node* base = ptr;
		for(U32 hops = 0; hops < detail::kMaxPtrHops; ++hops) {
			BinaryNode* step = dyn_cast<BinaryNode>(base);
			if(!step || (step->getOpcode() != Opcode::Add && step->getOpcode() != Opcode::Sub) ||
				 !step->getLHS()->getType()->isPtr())
				break;
			I64 sign = 1;
			if(step->getOpcode() == Opcode::Sub)
				sign = -1;
			detail::refineTerm(step->getRHS(), sign, false, offset, key, 0);
			base = step->getLHS();
		}
		detail::mergeTerms(key);
		B32 hasTerms = !key.empty();
		key.push_back({base, 0});
		Addr* entry = nullptr;
		if(hasTerms)
			entry = &termGroups.try_emplace(key).first->second;
		else
			entry = &baseGroups.try_emplace(base->getId()).first->second;
		if(!entry->group) {
			*entry = classifyBase(key);
			entry->group = (U32)(termGroups.size() + baseGroups.size());
		}
		Addr a = *entry;
		a.offset = offset;
		return a;
	}

	// object id of a group's base and whether it is a parameter
	// unknown when a term may carry a restrict parameter, as a + (d - a) is based on d
	Addr AddrCache::classifyBase(const detail::Terms& terms) {
		Addr a;
		const Node* base = terms.back().first;
		if(restrictParams)
			for(U32 i = 0; i + 1 < terms.size(); ++i) {
				const Node* var = terms[i].first;
				if(AliasAnalysis::mayCarryRestrict({var}, restrictParams, AliasAnalysis::kMaxCarryCone))
					return a;
			}
		I32 paramIndex = AliasAnalysis::paramIndex(base);
		a.param = paramIndex >= 0;
		if(const GlobalNode* global = dyn_cast<GlobalNode>(base)) {
			U32 number = symbols.try_emplace(global->getSymbol(), (U32)symbols.size()).first->second;
			a.object = detail::kObjectTags * number + detail::kGlobalTag;
		} else if(AliasAnalysis::isIdentified(base)) {
			a.object = detail::kObjectTags * base->getId() + detail::kAllocTag;
		} else if(AliasAnalysis::isRestrictParam(paramIndex, restrictParams)) {
			a.object = detail::kObjectTags * (U32)paramIndex + detail::kRestrictTag;
		}
		return a;
	}

	B32 disjointBases(const Addr& a, const Addr& b) {
		if(a.object == b.object)
			return false; // one object, or two bases without one
		if(a.object && b.object)
			return true;
		return a.param && b.param; // one side has an object, and a parameter with one is restrict
	}

	B32 disjoint(const Addr& a, U32 sizeA, const Addr& b, U32 sizeB) {
		if(a.group != b.group)
			return disjointBases(a, b);
		return a.group && (a.offset + (I64)sizeA <= b.offset || b.offset + (I64)sizeB <= a.offset);
	}
} // namespace rat
