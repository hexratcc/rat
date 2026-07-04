#include "Pass/Opt/AliasAnalysis.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	AliasAnalysis::AliasAnalysis(const Function& fn)
	: fn(fn) {}

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
		std::sort(info.symbolic.begin(), info.symbolic.end(), [](const Node* a, const Node* b) {
			return a->getId() < b->getId();
		});
		return decomposeCache.emplace(addr, std::move(info)).first->second;
	}

	U32 AliasAnalysis::getAccessSize(
			const Node* access) const { // byte size of a load/store (0 otherwise)
		Node* n = const_cast<Node*>(access);
		Type* t = nullptr;
		if(n->getOpcode() == Opcode::Load)
			t = n->getType();
		else if(n->getOpcode() == Opcode::Store)
			t = cast<StoreNode>(n)->getValue()->getType();
		else
			return 0;

		return t->byteSize(fn.getModule().pointerBytes());
	}

	AliasAnalysis::MustAliasKey AliasAnalysis::mustAliasKey(Node* access) const {
		auto addrOf = [](Node* n) -> Node* {
			if(n->getOpcode() == Opcode::Load)
				return cast<LoadNode>(n)->getPointer();
			if(n->getOpcode() == Opcode::Store)
				return cast<StoreNode>(n)->getPointer();
			return nullptr;
		};
		MustAliasKey key;
		Node* addr = addrOf(access);
		if(!addr)
			return key; // not a memory access
		const Address& a = decompose(addr);
		key.base = a.base;
		key.constant = a.constant;
		key.symbolic = a.symbolic;
		key.size = getAccessSize(access);
		return key; // valid if base known and size known
	}

	B32 AliasAnalysis::isIdentified(const Node* n) { return isa<AllocNode>(n) || isa<GlobalNode>(n); }

	B32 AliasAnalysis::distinctObjects(const Node* a, const Node* b) {
		// a and b are provably different
		if(!isIdentified(a) || !isIdentified(b))
			return false;
		if(isa<AllocNode>(a) || isa<AllocNode>(b))
			return a != b;
		return cast<GlobalNode>(a)->getSymbol() != cast<GlobalNode>(b)->getSymbol();
	}

	AliasResult AliasAnalysis::alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const {
		// alias query between two addresses with known access sizes (0 = unknown)
		const Address& a = decompose(addrA);
		const Address& b = decompose(addrB);

		// different base objects
		if(a.base != b.base)
			return distinctObjects(a.base, b.base) ? AliasResult::NoAlias : AliasResult::MayAlias;

		// same base, but the symbolic parts must match to compare offsets
		if(a.symbolic.size() != b.symbolic.size())
			return AliasResult::MayAlias;
		for(U32 i = 0, e = (U32)a.symbolic.size(); i < e; ++i)
			if(a.symbolic[i] != b.symbolic[i])
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

	AliasResult AliasAnalysis::alias(Node* accessA, Node* accessB) const {
		// alias query between two Load/Store accesses (sizes derived from them)
		auto addrOf = [](Node* n) -> Node* {
			if(n->getOpcode() == Opcode::Load)
				return cast<LoadNode>(n)->getPointer();
			if(n->getOpcode() == Opcode::Store)
				return cast<StoreNode>(n)->getPointer();
			return nullptr;
		};
		Node* pa = addrOf(accessA);
		Node* pb = addrOf(accessB);
		if(!pa || !pb)
			return AliasResult::MayAlias;
		return alias(pa, getAccessSize(accessA), pb, getAccessSize(accessB));
	}
} // namespace rat
