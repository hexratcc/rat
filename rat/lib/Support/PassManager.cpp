#include "Support/PassManager.h"

#include "IR/Module.h"

#include <iomanip>

namespace rat {
	Pass* PassManager::add(UniquePtr<Pass> pass) {
		Pass* raw = pass.get();
		passes.push_back(std::move(pass));
		return raw;
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
		auto runOne = [&](const C8* name, auto&& fn) {
			auto start = Clock::now();
			B32 c = fn();
			U64 nanos =
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			record(name, nanos);
			if(log)
				*log << "; " << name << (c ? " : changed\n" : " : unchanged\n");
			changed = changed || c;
		};
		for(auto& pass : passes)
			runOne(pass->name(), [&] { return pass->run(module); });
		for(auto& pass : machinePasses)
			runOne(pass->name(), [&] { return pass->run(module, mm, *target); });
		return changed;
	}

	List<PassTiming> PassManager::timings() const {
		List<PassTiming> sorted = timing;
		std::sort(sorted.begin(), sorted.end(), [](const PassTiming& a, const PassTiming& b) {
			return a.nanos > b.nanos;
		});
		return sorted;
	}

	void PassManager::printTimingReport(std::ostream& os) const {
		U64 total = 0;
		for(auto& t : timing)
			total += t.nanos;
		if(total == 0)
			total = 1;

		B32 first = true;
		for(auto& t : timings()) {
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
