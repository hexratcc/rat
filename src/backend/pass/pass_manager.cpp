#include "pass/pass_manager.h"

#include "ir/module.h"

#include <chrono>
#include <cstring>
#include <iomanip>

namespace rat {
	Pass* PassManager::add(UniquePtr<Pass> pass) {
		Pass* raw = pass.get();
		passes.push_back(std::move(pass));
		return raw;
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

	B32 PassManager::run(Module& module, std::ostream* log) {
		using Clock = std::chrono::steady_clock;
		B32 changed = false;
		List<B32> changedAt(passes.size(), false);
		auto runOne = [&](const C8* name, auto&& fn) -> B32 {
			auto start = Clock::now();
			B32 c = fn();
			U64 nanos =
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			record(name, nanos);
			if(log)
				*log << "; " << name << (c ? " : changed\n" : " : unchanged\n");
			changed = changed || c;
			return c;
		};
		auto runAt = [&](U32 i) {
			Pass* pass = passes[i].get();
			if(i < gated.size() && gated[i]) {
				I32 prev = -1;
				for(I32 j = (I32)i - 1; j >= 0; --j)
					if(std::strcmp(passes[(U32)j]->name(), pass->name()) == 0) {
						prev = j;
						break;
					}
				B32 due = prev < 0;
				for(I32 j = prev + 1; !due && j < (I32)i; ++j)
					due = changedAt[(U32)j];
				if(!due) {
					if(log)
						*log << "; " << pass->name() << " : skipped (no changes since last run)\n";
					changedAt[i] = false;
					return;
				}
			}
			changedAt[i] = runOne(pass->name(), [&] { return pass->run(module, *target); });
		};

		U32 n = (U32)passes.size();
		U32 loopEnd = fixpointEnd; // fixpoint covers [0, loopEnd); rest runs once
		if(fixpointEnd) {
			constexpr U32 kMaxSweeps = 32;
			B32 sweepChanged = true;
			for(U32 s = 0; s < kMaxSweeps && sweepChanged; ++s) {
				sweepChanged = false;
				for(B32& v : changedAt)
					v = false;
				for(U32 i = 0; i < loopEnd; ++i) {
					runAt(i);
					sweepChanged = sweepChanged || changedAt[i];
				}
			}
		}
		for(U32 i = loopEnd; i < n; ++i)
			runAt(i);
		for(auto& pass : machinePasses)
			runOne(pass->name(), [&] { return pass->run(module, mm, *target); });
		return changed;
	}

	void PassManager::printTimingReport(std::ostream& os) const {
		U64 total = 0;
		for(auto& t : timing)
			total += t.nanos;
		if(total == 0)
			total = 1;

		List<PassTiming> sorted = timing;
		std::sort(sorted.begin(), sorted.end(), [](const PassTiming& a, const PassTiming& b) {
			return a.nanos > b.nanos;
		});

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
