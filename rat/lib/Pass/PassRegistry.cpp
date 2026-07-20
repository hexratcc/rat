#include "Pass/PassRegistry.h"

#include "Pass/Pass.h"

#include "Pass/Verify.h"

#include "Pass/Emit/CEmitter.h"
#include "Pass/Emit/GraphEmitter.h"
#include "Pass/Emit/TextEmitter.h"

#include "Pass/Opt/DeadFuncElim.h"
#include "Pass/Opt/Fold.h"
#include "Pass/Opt/GVN.h"
#include "Pass/Opt/Inline.h"
#include "Pass/Opt/MemoryOpt.h"
#include "Pass/Opt/SCCP.h"
#include "Pass/Opt/SimplifyCFG.h"

namespace rat {
	void PassRegistry::add(String name, String description, Factory make) {
		items.push_back({std::move(name), std::move(description), make});
	}

	const PassRegistry::Entry* PassRegistry::find(const String& name) const {
		for(const Entry& e : items)
			if(e.name == name)
				return &e;
		return nullptr;
	}

	UniquePtr<Pass> PassRegistry::create(const String& name, std::ostream& out) const {
		const Entry* e = find(name);
		return e ? e->make(out) : nullptr;
	}

	void PassRegistry::registerAll(PassRegistry& r) {
		r.add<FoldPass>("fold", "constant folding and algebraic simplification");
		r.add<GVNPass>("gvn", "global value numbering");
		r.add<SCCPPass>("sccp", "sparse conditional constant propagation");
		r.add<SimplifyCFGPass>("simplifycfg", "control-flow simplification");
		r.add<MemoryOptPass>("memoryopt", "load/store forwarding");
		r.add<InlinePass>("inline", "function inlining");
		r.add<DeadFuncElimPass>("dfe", "dead (unreferenced internal) function elimination");
		r.add<VerifyPass>("verify", "edge consistency + structural invariants");
		r.add<TextEmitterPass>("text-emitter", "textual IR visualization");
		r.add<GraphEmitterPass>("graph-emitter", "Graphviz DOT IR visualization");
		r.add<CEmitterPass>("c-emitter", "C code generation");
	}

	PassRegistry& passRegistry() {
		static PassRegistry reg = [] {
			PassRegistry r;
			PassRegistry::registerAll(r);
			return r;
		}();
		return reg;
	}

	List<String> defaultOptPipeline() {
		return {"sccp", "fold", "simplifycfg", "gvn", "memoryopt", "inline", "fold", "gvn", "dfe"};
	}

	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err) {
		const PassRegistry& reg = passRegistry();
		String tok;
		auto flush = [&]() -> B32 {
			if(tok.empty())
				return true;
			UniquePtr<Pass> p = reg.create(tok, out);
			if(!p) {
				err = "unknown pass: " + tok;
				return false;
			}
			pm.add(std::move(p));
			tok.clear();
			return true;
		};
		for(C8 ch : spec) {
			if(ch == ',' || std::isspace((U8)ch)) {
				if(!flush())
					return false;
			} else {
				tok.push_back(ch);
			}
		}
		return flush();
	}
} // namespace rat
