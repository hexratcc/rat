#include "pass/opt/gvn.h"

#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/node.h"

namespace rat {
	namespace detail {
		B32 makeKey(Node* n, GVNKey& k) {
			k.op = (U32)n->getOpcode();
			k.type = n->getType()->getUid();
			switch(n->getOpcode()) {
			case Opcode::Constant:
				k.payload = cast<ConstantNode>(n)->getValue();
				return true;
			case Opcode::Global:
				k.sym = &cast<GlobalNode>(n)->getSymbol();
				return true;
			case Opcode::Extract:
				k.payload = cast<ExtractNode>(n)->getLane();
				break;
			case Opcode::Shuffle:
				k.payload = cast<ShuffleNode>(n)->getSelector();
				break;
			default:
				break;
			}
			U32 e = n->getInputCount();
			if(e > 2)
				return false;
			if(e > 0 && n->getInput(0))
				k.in0 = n->getInput(0)->getId();
			if(e > 1 && n->getInput(1))
				k.in1 = n->getInput(1)->getId();
			if(n->isCommutative() && k.in1 < k.in0)
				std::swap(k.in0, k.in1);
			return true;
		}
	} // namespace detail

	B32 GVNPass::isPureValue(Node* n) {
		Opcode op = n->getOpcode();
		B32 value = op == Opcode::Constant || op == Opcode::Global || isArithmeticOpcode(op) ||
								isVectorUtilOpcode(op);
		return value && !Schedule::mayTrap(n);
	}

	B32 GVNPass::hasKeyedUser(Node* n) const {
		for(Node* u : n->getUsers())
			if(keyed[u->getId()])
				return true;
		return false;
	}

	const C8* GVNPass::name() const { return "gvn"; }

	U32 GVNPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 removed = 0;

		U32 cap = 16;
		while(cap < fn.size() * 2)
			cap <<= 1;
		if(slots.size() < cap)
			slots.resize(cap);
		const U32 mask = cap - 1;
		detail::GVNKeyHash hasher;

		B32 changed = true;
		while(changed) {
			U32 before = removed;
			changed = false;
			for(U32 i = 0; i < cap; ++i)
				slots[i].val = nullptr;
			keyed.assign(fn.idBound(), 0);
			U32 filled = 0;
			for(Node* n : fn) {
				if(!isPureValue(n) || !n->hasUsers())
					continue;
				detail::GVNKey key;
				if(!detail::makeKey(n, key))
					continue;
				keyed[n->getId()] = 1;
				U32 i = (U32)hasher(key) & mask;
				while(slots[i].val && !(slots[i].key == key))
					i = (i + 1) & mask;
				if(!slots[i].val) {
					slots[i].key = key;
					slots[i].val = n;
					++filled;
					if(filled * 2 >= cap) {
						changed = removed != before;
						break;
					}
				} else {
					// n is a duplicate of the earlier representative; redirect its uses
					changed = changed || hasKeyedUser(n);
					n->replaceAllUsesWith(slots[i].val);
					++removed;
				}
			}
		}

		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
