#include "Pass/Opt/DeadFuncElim.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	const C8* DeadFuncElimPass::name() const { return "dfe"; }

	void DeadFuncElimPass::collectReferenced(Function& fn, Set<String>& referenced) {
		for(Node* n : fn) {
			if(CallNode* c = dyn_cast<CallNode>(n)) {
				if(!c->isIndirect())
					referenced.insert(c->getCallee());
			} else if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
				referenced.insert(g->getSymbol());
			}
		}
	}

	B32 DeadFuncElimPass::run(Module& module, const TargetInfo&) {
		B32 changed = false;
		for(;;) {
			Set<String> referenced;
			for(Function* fn : module)
				collectReferenced(*fn, referenced);
			for(const Global* g : module.globals())
				for(const Reloc& r : g->getRelocs())
					referenced.insert(r.symbol);

			Function* victim = nullptr;
			for(Function* fn : module) {
				if(!fn->isInternal())
					continue;
				if(referenced.find(fn->getName()) != referenced.end())
					continue;
				victim = fn;
				break;
			}
			if(!victim)
				break;
			module.removeFunction(victim);
			changed = true;
		}
		return changed;
	}
} // namespace rat
