#include "pass/pass_manager.h"

#include "analysis/call_graph.h"
#include "codegen/machine_function.h"
#include "ir/module.h"

#include <chrono>
#include <iomanip>
#include <ostream>

namespace rat {
	namespace detail {
		U64 nowNanos() {
			auto since = std::chrono::steady_clock::now().time_since_epoch();
			return std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
		}

		B32 slowerPass(const PassTiming& a, const PassTiming& b) { return a.nanos > b.nanos; }
	} // namespace detail

	Pass* PassManager::add(UniquePtr<Pass> pass) {
		return passes.emplace_back(std::move(pass)).get();
	}

	MachinePass* PassManager::add(UniquePtr<MachinePass> p) {
		return machinePasses.emplace_back(std::move(p)).get();
	}

	void PassManager::record(const C8* name, U64 nanos) {
		for(auto& t : timing) {
			if(t.name == name) {
				t.nanos += nanos;
				return;
			}
		}
		timing.push_back({name, nanos});
	}

	B32 PassManager::finish(const C8* name, U64 nanos, B32 changed, std::ostream* log) {
		record(name, nanos);
		if(log)
			*log << "; " << name << (changed ? " : changed\n" : " : unchanged\n");
		return changed;
	}

	U32 PassManager::functionPassRun(U32 first) const {
		U32 last = first;
		while(last < passes.size() && dynamic_cast<FunctionPass*>(passes[last].get()))
			++last;
		return last;
	}

	// passes [first, last) run function by function, callees first, so a callee is fully optimized
	// before any caller inlines it
	void PassManager::runFunctionPasses(U32 first, U32 last, Module& module, std::ostream* log) {
		List<FunctionPass*> group;
		for(U32 i = first; i < last; ++i)
			group.push_back(static_cast<FunctionPass*>(passes[i].get()));
		List<U64> nanos(group.size(), 0);
		List<B32> changed(group.size(), false);
		CallGraph graph;
		graph.build(module);
		for(FunctionPass* p : group)
			p->beginModule(module, graph);
		for(Function* fn : graph.bottomUp()) {
			for(U32 i = 0; i < group.size(); ++i) {
				U64 start = detail::nowNanos();
				changed[i] |= group[i]->runFunction(*fn, *target);
				nanos[i] += detail::nowNanos() - start;
			}
		}
		for(U32 i = 0; i < group.size(); ++i) {
			group[i]->endModule(module);
			finish(group[i]->name(), nanos[i], changed[i], log);
		}
	}

	void PassManager::run(Module& module, std::ostream* log) {
		for(U32 i = 0; i < passes.size();) {
			U32 last = functionPassRun(i);
			if(last > i) {
				runFunctionPasses(i, last, module, log);
				i = last;
				continue;
			}
			U64 start = detail::nowNanos();
			B32 changed = passes[i]->run(module, *target);
			finish(passes[i]->name(), detail::nowNanos() - start, changed, log);
			++i;
		}
		runMachine(module, log);
	}

	void PassManager::runMachine(Module& module, std::ostream* log) {
		List<B32> changed(machinePasses.size(), false);
		List<U64> nanos(machinePasses.size(), 0);
		MachineFunc mf;
		for(const Function* f : module) {
			mf.reset();
			for(U32 i = 0; i < machinePasses.size(); ++i) {
				U64 start = detail::nowNanos();
				changed[i] |= machinePasses[i]->run(module, *f, mf, *target);
				nanos[i] += detail::nowNanos() - start;
			}
		}
		for(U32 i = 0; i < machinePasses.size(); ++i) {
			U64 start = detail::nowNanos();
			machinePasses[i]->finish(module, *target);
			U64 total = nanos[i] + detail::nowNanos() - start;
			finish(machinePasses[i]->name(), total, changed[i], log);
		}
	}

	void PassManager::printTimingReport(std::ostream& os) const {
		U64 total = 0;
		for(auto& t : timing)
			total += t.nanos;
		if(total == 0)
			total = 1;

		List<PassTiming> sorted = timing;
		std::sort(sorted.begin(), sorted.end(), detail::slowerPass);

		B32 first = true;
		for(auto& t : sorted) {
			F64 pct = 100.0 * static_cast<F64>(t.nanos) / static_cast<F64>(total);
			F64 ms = static_cast<F64>(t.nanos) / 1e6;
			if(!first)
				os << ", ";
			os << t.name << " " << std::fixed << std::setprecision(3) << ms << "ms ("
				 << std::setprecision(2) << pct << "%)";
			first = false;
		}
		os << "\n";
	}
} // namespace rat
