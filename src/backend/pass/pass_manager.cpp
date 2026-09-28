#include "pass/pass_manager.h"

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

	void PassManager::gateLastOnChangesSinceSelf() {
		gated.resize(passes.size(), false);
		gated.back() = true;
	}

	void PassManager::record(const C8* name, U64 nanos) {
		for(auto& t : timing) {
			if(t.name == name) {
				t.nanos += nanos;
				++t.calls;
				return;
			}
		}
		timing.push_back({name, nanos, 1});
	}

	B32 PassManager::finish(const C8* name, U64 start, B32 changed, std::ostream* log) {
		record(name, detail::nowNanos() - start);
		if(log)
			*log << "; " << name << (changed ? " : changed\n" : " : unchanged\n");
		return changed;
	}

	B32 PassManager::isDue(U32 i, const List<B32>& changedAt) const {
		if(i >= gated.size() || !gated[i])
			return true;
		for(U32 j = i; j-- > 0;) {
			if(std::strcmp(passes[j]->name(), passes[i]->name()) == 0)
				return false;
			if(changedAt[j])
				return true;
		}
		return true;
	}

	void PassManager::runAt(U32 i, Module& module, List<B32>& changedAt, std::ostream* log) {
		Pass* pass = passes[i].get();
		const C8* name = pass->name();
		if(!isDue(i, changedAt)) {
			if(log)
				*log << "; " << name << " : skipped (no changes since last run)\n";
			changedAt[i] = false;
			return;
		}
		U64 start = detail::nowNanos();
		B32 changed = pass->run(module, *target);
		changedAt[i] = finish(name, start, changed, log);
	}

	void PassManager::run(Module& module, std::ostream* log) {
		List<B32> changedAt(passes.size(), false);
		U32 n = (U32)passes.size();
		U32 loopEnd = fixpointEnd; // fixpoint covers [0, loopEnd); rest runs once
		if(fixpointEnd) {
			constexpr U32 kMaxSweeps = 32;
			B32 sweepChanged = true;
			for(U32 s = 0; s < kMaxSweeps && sweepChanged; ++s) {
				sweepChanged = false;
				changedAt.assign(n, false);
				for(U32 i = 0; i < loopEnd; ++i) {
					runAt(i, module, changedAt, log);
					sweepChanged = sweepChanged || changedAt[i];
				}
			}
		}
		for(U32 i = loopEnd; i < n; ++i)
			runAt(i, module, changedAt, log);
		for(auto& pass : machinePasses) {
			U64 start = detail::nowNanos();
			B32 changed = pass->run(module, mm, *target);
			finish(pass->name(), start, changed, log);
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
