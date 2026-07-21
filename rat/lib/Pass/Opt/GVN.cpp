#include "Pass/Opt/GVN.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	namespace {
		struct GVNKey {
			U32 op = 0;
			U32 type = 0;
			I64 payload = 0;						 // constant value
			const String* sym = nullptr; // global symbol (interned in the node)
			U32 in0 = ~0u, in1 = ~0u;

			B32 operator==(const GVNKey& o) const {
				return op == o.op && type == o.type && payload == o.payload && in0 == o.in0 &&
							 in1 == o.in1 && (sym == o.sym || (sym && o.sym && *sym == *o.sym));
			}
		};

		struct GVNKeyHash {
			size_t operator()(const GVNKey& k) const {
				U64 h = 1469598103934665603ull;
				auto mix = [&](U64 v) {
					h ^= v;
					h *= 1099511628211ull;
				};
				mix(k.op);
				mix(k.type);
				mix((U64)k.payload);
				mix(k.in0);
				mix(((U64)k.in1) << 32);
				if(k.sym)
					mix(std::hash<String>{}(*k.sym));
				return (size_t)h;
			}
		};

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
			default:
				break;
			}
			U32 e = n->getInputCount();
			if(e > 2)
				return false;
			if(e > 0) {
				Node* a = n->getInput(0);
				k.in0 = a ? a->getId() : ~0u;
			}
			if(e > 1) {
				Node* b = n->getInput(1);
				k.in1 = b ? b->getId() : ~0u;
			}
			if(n->isCommutative() && k.in1 < k.in0)
				std::swap(k.in0, k.in1);
			return true;
		}
	} // namespace

	B32 GVNPass::isPureValue(Node* n) {
		Opcode op = n->getOpcode();
		return op == Opcode::Constant || op == Opcode::Global || isArithmeticOpcode(op);
	}

	const C8* GVNPass::name() const { return "gvn"; }

	U32 GVNPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 removed = 0;

		B32 changed = true;
		while(changed) {
			changed = false;
			std::unordered_map<GVNKey, Node*, GVNKeyHash> table;
			table.reserve(fn.size());
			for(Node* n : fn) {
				if(!GVNPass::isPureValue(n) || !n->hasUsers())
					continue;
				GVNKey key;
				if(!makeKey(n, key))
					continue;
				auto it = table.find(key);
				if(it == table.end()) {
					table.emplace(key, n);
				} else {
					// n is a duplicate of the earlier representative; redirect its uses
					n->replaceAllUsesWith(it->second);
					++removed;
					changed = true;
				}
			}
		}

		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
