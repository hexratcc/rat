#include "compile.h"

#include "rat.h"

#include <cstdio>
#include <cstdlib>

namespace rat::cc {
	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out) {
		if(!opt.renameMain.empty())
			pm.add<RenameSymbolPass>("main", opt.renameMain);

		Set<String> seen;
		for(UniquePtr<Pass>& p : opt.optPasses) {
			String name = p->name();
			pm.add(std::move(p));
			if(!seen.insert(name).second)
				pm.gateLastOnChangesSinceSelf();
		}

		pm.markFixpointEnd();

		if(!opt.machinePasses.empty()) {
			for(UniquePtr<MachinePass>& p : opt.machinePasses)
				pm.add(std::move(p));
		} else {
			pm.add<X86LowerPass>();
			pm.add<LinearScanRegAllocPass>();
			pm.add<X86LayoutPass>();
			pm.add<X86EncodePass>(out);
		}
	}

	void
	compileModule(Module& mod, const TargetInfo& target, CompileOptions& opt, std::ostream& out) {
		PassManager pm(target);
		composePipeline(pm, opt, out);
		pm.run(mod);
	}
} // namespace rat::cc
