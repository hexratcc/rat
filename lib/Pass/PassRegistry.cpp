#include "Pass/PassRegistry.h"

#include "Pass/Pass.h"

#include "Pass/Verify.h"

#include "Pass/Emit/CEmitter.h"
#include "Pass/Emit/GraphEmitter.h"
#include "Pass/Emit/TextEmitter.h"

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
		for (const Entry& e : items)
			if (e.name == name)
				return &e;
		return nullptr;
	}

	UniquePtr<Pass> PassRegistry::create(const String& name,
																			 std::ostream& out) const {
		const Entry* e = find(name);
		return e ? e->make(out) : nullptr;
	}

	namespace {
		void registerAllPasses(PassRegistry& r) {
			r.add("fold", "constant folding and algebraic simplification",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<FoldPass>();
						});
			r.add("gvn", "global value numbering",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<GVNPass>();
						});
			r.add("sccp", "sparse conditional constant propagation",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<SCCPPass>();
						});
			r.add("simplifycfg", "control-flow simplification",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<SimplifyCFGPass>();
						});
			r.add("memoryopt", "load/store forwarding",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<MemoryOptPass>();
						});
			r.add("inline", "function inlining",
						[](std::ostream&) -> UniquePtr<Pass> {
							return std::make_unique<InlinePass>();
						});
			r.add("verify", "edge consistency + structural invariants",
						[](std::ostream& os) -> UniquePtr<Pass> {
							return std::make_unique<VerifyPass>(os);
						});
			r.add("text-emitter", "textual IR visualization",
						[](std::ostream& os) -> UniquePtr<Pass> {
							return std::make_unique<TextEmitterPass>(os);
						});
			r.add("graph-emitter", "Graphviz DOT IR visualization",
						[](std::ostream& os) -> UniquePtr<Pass> {
							return std::make_unique<GraphEmitterPass>(os);
						});
			r.add("c-emitter", "C code generation",
						[](std::ostream& os) -> UniquePtr<Pass> {
							return std::make_unique<CEmitterPass>(os);
						});
		}
	} // namespace

	PassRegistry& passRegistry() {
		static PassRegistry reg = [] {
			PassRegistry r;
			registerAllPasses(r);
			return r;
		}();
		return reg;
	}

	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out,
										String& err) {
		const PassRegistry& reg = passRegistry();
		String tok;
		auto flush = [&]() -> B32 {
			if (tok.empty())
				return true;
			UniquePtr<Pass> p = reg.create(tok, out);
			if (!p) {
				err = "unknown pass: " + tok;
				return false;
			}
			pm.add(std::move(p));
			tok.clear();
			return true;
		};
		for (char ch : spec) {
			if (ch == ',' || std::isspace((unsigned char)ch)) {
				if (!flush())
					return false;
			} else {
				tok.push_back(ch);
			}
		}
		return flush();
	}
} // namespace rat
