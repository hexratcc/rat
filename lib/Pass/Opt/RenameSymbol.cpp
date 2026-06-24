#include "Pass/Opt/RenameSymbol.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	RenameSymbolPass::RenameSymbolPass(String from, String to)
			: from(std::move(from)), to(std::move(to)) {}

	const C8* RenameSymbolPass::name() const { return "rename-symbol"; }

	B32 RenameSymbolPass::run(Module& module) {
		if (from.empty() || from == to)
			return false;

		B32 changed = false;
		for (Function* fn : module) {
			if (fn->getName() == from) {
				fn->setName(to);
				changed = true;
			}
			for (Node* n : *fn) {
				CallNode* c = dyn_cast<CallNode>(n);
				if (c && !c->isIndirect() && c->getCallee() == from) {
					c->setCallee(to);
					changed = true;
				}
			}
		}
		return changed;
	}
} // namespace rat
