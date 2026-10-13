#include "analysis/alias_analysis.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	namespace detail {
		B32 idLess(const Node* a, const Node* b) { return a->getId() < b->getId(); }
	} // namespace detail

	AliasAnalysis::AliasAnalysis(U32 pointerBytes, U64 restrictParams)
	: ptrBytes(pointerBytes),
		restrictParams(restrictParams) {}

	const AliasAnalysis::Address& AliasAnalysis::decompose(Node* addr) const {
		auto it = decomposeCache.find(addr);
		if(it != decomposeCache.end())
			return it->second;

		Address info{addr, 0, {}};
		while(BinaryNode* b = dyn_cast<BinaryNode>(info.base)) {
			Opcode op = b->getOpcode();
			B32 isAddSub = (op == Opcode::Add || op == Opcode::Sub);
			if(!isAddSub || !b->getType()->isPtr() || !b->getLHS()->getType()->isPtr())
				break; // not pointer +/- integer arithmetic
			Node* off = b->getRHS();
			if(ConstantNode* co = dyn_cast<ConstantNode>(off)) {
				I64 v = co->getValue();
				info.constant += (op == Opcode::Add) ? v : -v;
			} else if(op == Opcode::Add) {
				info.symbolic.push_back(off);
			} else {
				break; // symbolic subtract: treat the whole node as an opaque base
			}
			info.base = b->getLHS();
		}
		std::sort(info.symbolic.begin(), info.symbolic.end(), detail::idLess);
		return decomposeCache.emplace(addr, std::move(info)).first->second;
	}

	// byte size of a load/store (0 otherwise)
	U32 AliasAnalysis::getAccessSize(const Node* access) const {
		if(access->getOpcode() == Opcode::Load)
			return access->getType()->byteSize(ptrBytes);
		if(access->getOpcode() == Opcode::Store)
			return cast<StoreNode>(access)->getValue()->getType()->byteSize(ptrBytes);
		return 0;
	}

	Node* AliasAnalysis::accessAddress(Node* n) {
		if(n->getOpcode() == Opcode::Load)
			return cast<LoadNode>(n)->getPointer();
		if(n->getOpcode() == Opcode::Store)
			return cast<StoreNode>(n)->getPointer();
		return nullptr;
	}

	AliasAnalysis::MustAliasKey AliasAnalysis::mustAliasKey(Node* access) const {
		MustAliasKey key;
		Node* addr = accessAddress(access);
		if(!addr)
			return key; // not a memory access
		const Address& a = decompose(addr);
		key.base = a.base;
		key.constant = a.constant;
		key.symbolic = a.symbolic;
		key.size = getAccessSize(access);
		return key; // valid if base known and size known
	}

	B32 AliasAnalysis::isIdentified(const Node* n) {
		return n && (isa<AllocNode>(n) || isa<StackAllocNode>(n) || isa<GlobalNode>(n));
	}

	B32 AliasAnalysis::distinctObjects(const Node* a, const Node* b) {
		// a and b are provably different
		if(!isIdentified(a) || !isIdentified(b))
			return false;
		if(!isa<GlobalNode>(a) || !isa<GlobalNode>(b))
			return a != b;
		return cast<GlobalNode>(a)->getSymbol() != cast<GlobalNode>(b)->getSymbol();
	}

	I32 AliasAnalysis::paramIndex(const Node* n) {
		const ProjNode* p = dyn_cast<ProjNode>(n);
		U32 first = StartNode::paramProjIndex(0);
		if(!p || !isa<StartNode>(p->getProducer()) || p->getIndex() < first)
			return -1;
		return (I32)(p->getIndex() - first);
	}

	B32 AliasAnalysis::isRestrictParam(I32 index, U64 restricts) {
		return index >= 0 && index < (I32)kMaxRestrictParams && (restricts >> index & 1);
	}

	B32 AliasAnalysis::mayCarryRestrict(List<const Node*> work, U64 restricts, U32 cap) {
		Set<const Node*> seen;
		while(!work.empty()) {
			const Node* c = work.back();
			work.pop_back();
			if(!c || !seen.insert(c).second)
				continue;
			if(seen.size() > cap)
				return true;
			Opcode op = c->getOpcode();
			I32 index = paramIndex(c);
			if(isRestrictParam(index, restricts))
				return true;
			if(index >= 0 || op == Opcode::Constant || op == Opcode::Load || op == Opcode::Proj ||
				 isIdentified(c))
				continue;
			B32 pure = isArithmeticOpcode(op) || isVectorUtilOpcode(op) || op == Opcode::Select ||
								 op == Opcode::Phi;
			if(!pure)
				return true;
			for(U32 i = 0; i < c->getInputCount(); ++i)
				if(const Node* in = c->getInput(i); in && in->getType() && in->getType()->isData())
					work.push_back(in);
		}
		return false;
	}

	U64 AliasAnalysis::restrictFacts(const Function& fn) {
		U64 restricts = fn.getAttrs().restrictParams;
		if(!restricts)
			return 0;
		List<const Node*> taken;
		for(Node* n : fn) {
			if(StoreNode* s = dyn_cast<StoreNode>(n); s && s->getInputCount())
				taken.push_back(s->getValue());
			else if(isa<CallNode>(n) || isa<AsmNode>(n))
				for(U32 i = 2; i < n->getInputCount(); ++i)
					taken.push_back(n->getInput(i));
		}
		if(mayCarryRestrict(std::move(taken), restricts, ~0u))
			return 0;
		return restricts;
	}

	Node* AliasAnalysis::objectOf(Node* addr) const {
		const Address& a = decompose(addr);
		Node* base = a.base;
		I32 index = paramIndex(base);
		B32 isRestrict = isRestrictParam(index, restrictParams);
		if(!isRestrict && !isIdentified(base))
			return nullptr;
		for(Node* t : a.symbolic)
			if(restrictParams && mayCarryRestrict({t}, restrictParams, kMaxCarryCone))
				return nullptr;
		if(isRestrict) {
			if(!paramObjects[index])
				paramObjects[index] = base;
			return paramObjects[index];
		}
		GlobalNode* g = dyn_cast<GlobalNode>(base);
		if(!g)
			return base;
		return symbolObjects.try_emplace(g->getSymbol(), base).first->second;
	}

	AliasResult AliasAnalysis::alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const {
		// alias query between two addresses with known access sizes (0 = unknown)
		const Address& a = decompose(addrA);
		const Address& b = decompose(addrB);

		// different base objects
		if(a.base != b.base)
			return distinctObjects(a.base, b.base) ? AliasResult::NoAlias : AliasResult::MayAlias;

		// same base, but the symbolic parts must match to compare offsets
		if(a.symbolic != b.symbolic)
			return AliasResult::MayAlias;

		// the two addresses differ only by a constant byte offset
		I64 delta = (I64)((U64)a.constant - (U64)b.constant); // a = b + delta (wraps)
		if(delta == 0)
			return AliasResult::MustAlias;

		B32 diffSigns = (a.constant < 0) != (b.constant < 0);
		if(diffSigns && (delta < 0) != (a.constant < 0))
			return AliasResult::MayAlias;

		if(sizeA == 0 || sizeB == 0)
			return AliasResult::MayAlias; // unknown size: cannot prove disjoint

		// disjoint if a starts at/after b's end, or b starts at/after a's end
		if(delta >= (I64)sizeB || -delta >= (I64)sizeA)
			return AliasResult::NoAlias;
		return AliasResult::MayAlias;
	}
} // namespace rat
