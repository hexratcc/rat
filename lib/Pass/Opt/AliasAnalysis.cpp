// alias analysis: decide whether two memory accesses may, must, or never
// refer to overlapping storage, used to drive MemoryOpt
//
// references:
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#include "Pass/Opt/AliasAnalysis.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

#include <algorithm>

namespace rat {
	const char* toString(AliasResult r) {
		switch (r) {
		case AliasResult::NoAlias:
			return "NoAlias";
		case AliasResult::MayAlias:
			return "MayAlias";
		case AliasResult::MustAlias:
			return "MustAlias";
		}
		return "?";
	}

	AliasAnalysis::AliasAnalysis(const Function& fn) : fn(fn) {}

	AliasAnalysis::AddrInfo AliasAnalysis::decompose(Node* addr) const {
		AddrInfo info{addr, 0, {}};
		while (BinaryNode* b = dyn_cast<BinaryNode>(info.base)) {
			Opcode op = b->getOpcode();
			B32 isAddSub = (op == Opcode::Add || op == Opcode::Sub);
			if (!isAddSub || !b->getType()->isPtr() ||
					!b->getLHS()->getType()->isPtr())
				break; // not pointer +/- integer arithmetic
			Node* off = b->getRHS();
			if (ConstantNode* co = dyn_cast<ConstantNode>(off)) {
				I64 v = co->getValue();
				info.constant += (op == Opcode::Add) ? v : -v;
			} else if (op == Opcode::Add) {
				info.symbolic.push_back(off);
			} else {
				break; // symbolic subtract: treat the whole node as an opaque base
			}
			info.base = b->getLHS();
		}
		std::sort(
				info.symbolic.begin(), info.symbolic.end(),
				[](const Node* a, const Node* b) { return a->getId() < b->getId(); });
		return info;
	}

	U32 AliasAnalysis::accessSize(const Node* access) const {
		Node* n = const_cast<Node*>(access);
		Type* t = nullptr;
		if (n->getOpcode() == Opcode::Load)
			t = n->getType();
		else if (n->getOpcode() == Opcode::Store)
			t = cast<StoreNode>(n)->getValue()->getType();
		else
			return 0;

		return t->byteSize(fn.getModule().pointerBytes());
	}

	namespace detail {
		B32 isIdentified(const Node* n) {
			return isa<AllocNode>(n) || isa<GlobalNode>(n);
		}

		B32 distinctObjects(const Node* a, const Node* b) {
			if (!isIdentified(a) || !isIdentified(b))
				return false;

			if (isa<AllocNode>(a) || isa<AllocNode>(b))
				return a != b;
			return cast<GlobalNode>(a)->getSymbol() !=
						 cast<GlobalNode>(b)->getSymbol();
		}
	} // namespace detail
	using namespace detail;

	AliasResult AliasAnalysis::alias(Node* addrA, U32 sizeA, Node* addrB,
																	 U32 sizeB) const {
		AddrInfo a = decompose(addrA);
		AddrInfo b = decompose(addrB);

		// different (or unknown) base objects
		if (a.base != b.base)
			return distinctObjects(a.base, b.base) ? AliasResult::NoAlias
																						 : AliasResult::MayAlias;

		// same base, but the symbolic parts must match to compare offsets
		if (a.symbolic.size() != b.symbolic.size())
			return AliasResult::MayAlias;
		for (U32 i = 0, e = (U32)a.symbolic.size(); i < e; ++i)
			if (a.symbolic[i] != b.symbolic[i])
				return AliasResult::MayAlias;

		// the two addresses differ only by a constant byte offset
		I64 delta = a.constant - b.constant; // a = b + delta
		if (delta == 0)
			return AliasResult::MustAlias;

		if (sizeA == 0 || sizeB == 0)
			return AliasResult::MayAlias; // unknown size: cannot prove disjoint

		// disjoint iff a starts at/after b's end, or b starts at/after a's end
		if (delta >= (I64)sizeB || -delta >= (I64)sizeA)
			return AliasResult::NoAlias;
		return AliasResult::MayAlias;
	}

	AliasResult AliasAnalysis::alias(Node* accessA, Node* accessB) const {
		auto addrOf = [](Node* n) -> Node* {
			if (n->getOpcode() == Opcode::Load)
				return cast<LoadNode>(n)->getPointer();
			if (n->getOpcode() == Opcode::Store)
				return cast<StoreNode>(n)->getPointer();
			return nullptr;
		};
		Node* pa = addrOf(accessA);
		Node* pb = addrOf(accessB);
		if (!pa || !pb)
			return AliasResult::MayAlias;
		return alias(pa, accessSize(accessA), pb, accessSize(accessB));
	}
} // namespace rat
