#include "test_harness.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace rat {
	namespace detail {
		B32 hasSuffix(const String& s, const C8* suffix) {
			String suf = suffix;
			return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		}

		void collectCases(const String& dir, const C8* ext, List<String>& out) {
			std::error_code ec;
			List<String> subdirs;
			List<String> files;
			for(const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				String path = e.path().generic_string();
				if(e.is_directory(ec))
					subdirs.push_back(path);
				else if(hasSuffix(path, ext))
					files.push_back(path);
			}
			std::sort(files.begin(), files.end());
			std::sort(subdirs.begin(), subdirs.end());
			out.insert(out.end(), files.begin(), files.end());
			for(const String& s : subdirs)
				collectCases(s, ext, out);
		}

		String findDir(const List<String>& candidates) {
			std::error_code ec;
			for(const String& c : candidates)
				if(std::filesystem::is_directory(c, ec))
					return c;
			return "";
		}

		void SuiteRun::work(const TestSuiteSpec& spec, const List<String>& cases) {
			for(U32 i = next++; i < cases.size(); i = next++) {
				String err;
				B32 ok = false;
				try {
					ok = spec.run(cases[i], err);
				} catch(const std::exception& e) {
					err = String("unhandled exception: ") + e.what();
				} catch(...) {
					err = "unhandled exception";
				}
				std::lock_guard<std::mutex> lock(ioMtx);
				if(ok) {
					std::cout << "PASS  " << cases[i] << "\n";
					++passed;
				} else {
					std::cout << "FAIL  " << cases[i] << ": " << err << "\n";
					failures.push_back(cases[i]);
				}
			}
		}
	} // namespace detail

	I32 runTestSuite(I32 argc, C8** argv, const TestSuiteSpec& spec) {
		U32 jobs = 1;
		B32 quiet = false; // -q, only the per-case lines, the caller tallies them
		List<String> cases;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			if(arg == "-q") {
				quiet = true;
			} else if(arg.rfind("-j", 0) == 0) {
				String num = arg.substr(2);
				if(num.empty() && i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
					num = argv[++i];
				I64 requestedJobs = std::strtol(num.c_str(), nullptr, 10);
				jobs = (U32)std::max<I64>(1, requestedJobs);
			} else {
				cases.push_back(arg);
			}
		}
		if(cases.empty())
			detail::collectCases(detail::findDir(spec.dirCandidates), spec.extension, cases);
		if(cases.empty()) {
			std::cerr << spec.tool << ": no case paths given and no case directory found\n";
			return 2;
		}

		detail::SuiteRun run;
		List<std::thread> pool;
		for(U32 t = 1; t < std::min<U64>(jobs, cases.size()); ++t)
			pool.emplace_back(&detail::SuiteRun::work, &run, std::cref(spec), std::cref(cases));
		run.work(spec, cases);
		for(std::thread& t : pool)
			t.join();

		if(!quiet && !run.failures.empty()) {
			std::sort(run.failures.begin(), run.failures.end());
			std::cout << "\n=== failures ===\n";
			for(const String& path : run.failures)
				std::cout << "FAIL  " << path << "\n";
		}
		if(!quiet)
			std::cout << "\n" << run.passed << " passed, " << run.failures.size() << " failed\n";
		return run.failures.empty() ? 0 : 1;
	}
} // namespace rat
