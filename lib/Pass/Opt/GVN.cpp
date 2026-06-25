#include "Pass/Opt/GVN.h"

#include "IR/Function.h"
#include "IR/Node.h"

namespace rat {
	B32 GVNPass::isPureValue(Node* n) {
		Opcode op = n->getOpcode();
		return op == Opcode::Constant || op == Opcode::Global || isArithmeticOpcode(op);
	}

	const C8* GVNPass::name() const { return "gvn"; }

	U32 GVNPass::runOnFunction(Function& fn) {
		U32 removed = 0;

		B32 changed = true;
		while(changed) {
			changed = false;
			Map<String, Node*> table;
			for(Node* n : fn) {
				if(!GVNPass::isPureValue(n) || !n->hasUsers())
					continue;
				String sig = nodeSignature(n);
				auto it = table.find(sig);
				if(it == table.end()) {
					table.emplace(std::move(sig), n);
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
