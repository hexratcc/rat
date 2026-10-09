#include "pass/pass.h"

#include "analysis/call_graph.h"
#include "ir/module.h"

namespace rat {
	Pass::~Pass() = default;
	MachinePass::~MachinePass() = default;
	void MachinePass::finish(Module&, const TargetInfo&) {}

	B32 FunctionPass::run(Module& module, const TargetInfo& target) {
		CallGraph graph;
		graph.build(module);
		beginModule(module, graph);
		B32 changed = false;
		for(Function* fn : module)
			changed |= runFunction(*fn, target);
		endModule(module);
		return changed;
	}

	B32 FunctionPass::runFunction(Function& fn, const TargetInfo& target) {
		B32 skippable = onlyReadsFunction();
		if(skippable && fn.isCleanFor(name()))
			return false;
		if(runOnFunction(fn, target))
			return true;
		if(skippable)
			fn.markCleanFor(name());
		return false;
	}
} // namespace rat
