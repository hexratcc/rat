#include "pass/pass_registry.h"

#include <iomanip>

#include "pass/pass.h"
#include "string.h"

#include "pass/verify.h"

#include "codegen/linear_scan_reg_alloc.h"
#include "pass/emit/c_emitter.h"
#include "pass/emit/graph_emitter.h"
#include "pass/emit/text_emitter.h"
#include "pass/emit/x86_encode.h"
#include "pass/emit/x86_layout.h"
#include "pass/emit/x86_lower.h"

#include "pass/opt/dead_func_elim.h"
#include "pass/opt/fold.h"
#include "pass/opt/gvn.h"
#include "pass/opt/inline.h"
#include "pass/opt/memory_opt.h"
#include "pass/opt/sccp.h"
#include "pass/opt/simplify_cfg.h"
#include "pass/opt/slp_pack.h"
#include "pass/opt/strength_reduce.h"

namespace rat {
	namespace {
		template <typename B, typename P> UniquePtr<B> mk(std::ostream& os) {
			if constexpr(std::is_constructible_v<P, std::ostream&>)
				return std::make_unique<P>(os);
			else
				return std::make_unique<P>();
		}

		struct Entry {
			const C8 *name, *description;
			UniquePtr<Pass> (*make)(std::ostream&);
		};

		struct MachineEntry {
			const C8 *name, *description;
			UniquePtr<MachinePass> (*make)(std::ostream&);
		};

		constexpr Entry kPasses[] = {
				{"fold", "constant folding and algebraic simplification", &mk<Pass, FoldPass>},
				{"gvn", "global value numbering", &mk<Pass, GVNPass>},
				{"sccp", "sparse conditional constant propagation", &mk<Pass, SCCPPass>},
				{"simplifycfg", "control-flow simplification", &mk<Pass, SimplifyCFGPass>},
				{"memoryopt", "load/store forwarding", &mk<Pass, MemoryOptPass>},
				{"inline", "function inlining", &mk<Pass, InlinePass>},
				{"strengthreduce", "induction-variable strength reduction", &mk<Pass, StrengthReducePass>},
				{"slp", "scheduling-free superword (SLP) vectorization", &mk<Pass, SlpPackPass>},
				{"dfe", "dead (unreferenced internal) function elimination", &mk<Pass, DeadFuncElimPass>},
				{"verify", "edge consistency + structural invariants", &mk<Pass, VerifyPass>},
				{"text-emitter", "textual IR visualization", &mk<Pass, TextEmitterPass>},
				{"graph-emitter", "Graphviz DOT IR visualization", &mk<Pass, GraphEmitterPass>},
				{"c-emitter", "C code generation", &mk<Pass, CEmitterPass>},
		};

		// the default x86 machine pipeline, in order
		constexpr MachineEntry kMachinePasses[] = {
				{"x86-lower", "lower IR to x86 machine instructions", &mk<MachinePass, X86LowerPass>},
				{"regalloc", "linear-scan register allocation", &mk<MachinePass, LinearScanRegAllocPass>},
				{"x86-layout", "block ordering and frame layout", &mk<MachinePass, X86LayoutPass>},
				{"x86-encode", "instruction encoding and object emission", &mk<MachinePass, X86EncodePass>},
		};

		// the -O1 pipeline; repeats gate on changes since their last run
		const C8* kDefaultOpt = "sccp fold simplifycfg gvn memoryopt inline fold gvn"
														" strengthreduce fold gvn dfe";
	} // namespace

	UniquePtr<Pass> createPass(const String& name, std::ostream& out) {
		for(const Entry& e : kPasses)
			if(name == e.name)
				return e.make(out);
		return nullptr;
	}

	UniquePtr<MachinePass> createMachinePass(const String& name, std::ostream& out) {
		for(const MachineEntry& e : kMachinePasses)
			if(name == e.name)
				return e.make(out);
		return nullptr;
	}

	List<String> defaultOptPipeline() { return splitTokens(kDefaultOpt); }

	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err) {
		for(const String& name : splitTokens(spec)) {
			UniquePtr<Pass> p = createPass(name, out);
			if(!p)
				return err = "unknown pass '" + name + "'", false;
			pm.add(std::move(p));
		}
		return true;
	}

	void listPasses(std::ostream& os, B32 withMachine) {
		os << "passes:\n";
		for(const Entry& e : kPasses)
			os << "  " << std::left << std::setw(16) << e.name << e.description << "\n";
		if(!withMachine)
			return;
		os << "machine passes (default x86 pipeline order):\n";
		for(const MachineEntry& e : kMachinePasses)
			os << "  " << std::left << std::setw(16) << e.name << e.description << "\n";
	}
} // namespace rat
