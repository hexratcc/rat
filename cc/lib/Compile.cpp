#include "Compile.h"

#include "rat.h"

namespace rat::cc {
	List<UniquePtr<Pass>> defaultOptPasses() {
		List<UniquePtr<Pass>> passes;
		passes.push_back(std::make_unique<SCCPPass>());
		passes.push_back(std::make_unique<FoldPass>());
		passes.push_back(std::make_unique<SimplifyCFGPass>());
		passes.push_back(std::make_unique<GVNPass>());
		passes.push_back(std::make_unique<MemoryOptPass>());
		passes.push_back(std::make_unique<InlinePass>());
		passes.push_back(std::make_unique<FoldPass>());
		passes.push_back(std::make_unique<GVNPass>());
		return passes;
	}

	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out) {
		if(!opt.renameMain.empty())
			pm.add<RenameSymbolPass>("main", opt.renameMain);

		for(UniquePtr<Pass>& p : opt.optPasses)
			pm.add(std::move(p));
		opt.optPasses.clear();

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
