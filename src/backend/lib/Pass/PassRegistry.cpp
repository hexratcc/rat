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
#include "Pass/Opt/StrengthReduce.h"

namespace rat {
	namespace {
		template <typename P> UniquePtr<Pass> makeRegistered(std::ostream& os) {
			(void)os;
			if constexpr(std::is_constructible_v<P, std::ostream&>)
				return std::make_unique<P>(os);
			else
				return std::make_unique<P>();
		}

		constexpr PassRegistry::Entry kPasses[] = {
				{"fold", "constant folding and algebraic simplification", &makeRegistered<FoldPass>},
				{"gvn", "global value numbering", &makeRegistered<GVNPass>},
				{"sccp", "sparse conditional constant propagation", &makeRegistered<SCCPPass>},
				{"simplifycfg", "control-flow simplification", &makeRegistered<SimplifyCFGPass>},
				{"memoryopt", "load/store forwarding", &makeRegistered<MemoryOptPass>},
				{"inline", "function inlining", &makeRegistered<InlinePass>},
				{"strengthreduce",
				 "loop induction-variable strength reduction",
				 &makeRegistered<StrengthReducePass>},
				{"dfe",
				 "dead (unreferenced internal) function elimination",
				 &makeRegistered<DeadFuncElimPass>},
				{"verify", "edge consistency + structural invariants", &makeRegistered<VerifyPass>},
				{"text-emitter", "textual IR visualization", &makeRegistered<TextEmitterPass>},
				{"graph-emitter", "Graphviz DOT IR visualization", &makeRegistered<GraphEmitterPass>},
				{"c-emitter", "C code generation", &makeRegistered<CEmitterPass>},
		};
		constexpr U32 kPassCount = (U32)(sizeof(kPasses) / sizeof(kPasses[0]));

		// the -O1 pipeline, as direct constructors: no names, no lookups.
		template <typename P> UniquePtr<Pass> makeOpt() { return std::make_unique<P>(); }

		struct PipelineStep {
			const C8* name;
			UniquePtr<Pass> (*make)();
		};

		constexpr PipelineStep kDefaultPipeline[] = {
				{"sccp", &makeOpt<SCCPPass>},
				{"fold", &makeOpt<FoldPass>},
				{"simplifycfg", &makeOpt<SimplifyCFGPass>},
				{"gvn", &makeOpt<GVNPass>},
				{"memoryopt", &makeOpt<MemoryOptPass>},
				{"inline", &makeOpt<InlinePass>},
				{"fold", &makeOpt<FoldPass>},
				{"gvn", &makeOpt<GVNPass>},
				{"strengthreduce", &makeOpt<StrengthReducePass>},
				{"fold", &makeOpt<FoldPass>},
				{"gvn", &makeOpt<GVNPass>},
				{"dfe", &makeOpt<DeadFuncElimPass>},
		};
		constexpr U32 kDefaultPipelineLen =
				(U32)(sizeof(kDefaultPipeline) / sizeof(kDefaultPipeline[0]));
	} // namespace

	const PassRegistry::Entry* PassRegistry::find(const String& name) {
		for(const Entry& e : kPasses)
			if(name == e.name)
				return &e;
		return nullptr;
	}

	UniquePtr<Pass> PassRegistry::create(const String& name, std::ostream& out) const {
		const Entry* e = find(name);
		return e ? e->make(out) : nullptr;
	}

	PassRegistry::EntryTable PassRegistry::entries() { return {kPasses, kPassCount}; }

	const PassRegistry& passRegistry() {
		static const PassRegistry reg;
		return reg;
	}

	List<UniquePtr<Pass>> makeDefaultOptPasses() {
		List<UniquePtr<Pass>> passes;
		passes.reserve(kDefaultPipelineLen);
		for(const PipelineStep& s : kDefaultPipeline)
			passes.push_back(s.make());
		return passes;
	}

	List<String> defaultOptPipeline() {
		List<String> names;
		names.reserve(kDefaultPipelineLen);
		for(const PipelineStep& s : kDefaultPipeline)
			names.push_back(s.name);
		return names;
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
