#include "Compile.h"

#include "Host.h"

#include "rat.h"

#include <cstdio>
#include <cstdlib>

namespace rat::cc {
	List<UniquePtr<Pass>> defaultOptPasses() {
		std::ostringstream sink;
		List<UniquePtr<Pass>> passes;
		for(const String& name : defaultOptPipeline())
			passes.push_back(passRegistry().create(name, sink));
		return passes;
	}

	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out) {
		if(!opt.renameMain.empty())
			pm.add<RenameSymbolPass>("main", opt.renameMain);

		for(UniquePtr<Pass>& p : opt.optPasses)
			pm.add(std::move(p));

		if(opt.backend == Backend::C) {
			pm.add<CEmitterPass>(out);
		} else {
			pm.add<X86LowerPass>();
			if(opt.regAlloc == RegAlloc::Graph)
				pm.add<GraphColorRegAllocPass>();
			else
				pm.add<LinearScanRegAllocPass>();
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
