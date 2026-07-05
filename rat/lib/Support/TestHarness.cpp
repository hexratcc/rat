#include "Support/TestHarness.h"

namespace rat {
	namespace {
		B32 hasSuffix(const String& s, const char* suffix) {
			String suf = suffix;
			return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		}

		void collectCases(const String& dir, const char* ext, List<String>& out) {
			DIR* d = opendir(dir.c_str());
			if(!d)
				return;
			List<String> subdirs;
			List<String> files;
			for(struct dirent* e; (e = readdir(d));) {
				String name = e->d_name;
				if(name == "." || name == "..")
					continue;
				String path = dir + "/" + name;
				struct stat st;
				if(stat(path.c_str(), &st) != 0)
					continue;
				if(S_ISDIR(st.st_mode))
					subdirs.push_back(path);
				else if(hasSuffix(name, ext))
					files.push_back(path);
			}
			closedir(d);
			std::sort(files.begin(), files.end());
			std::sort(subdirs.begin(), subdirs.end());
			for(const String& f : files)
				out.push_back(f);
			for(const String& s : subdirs)
				collectCases(s, ext, out);
		}

		String findDir(const List<String>& candidates) {
			for(const String& c : candidates) {
				struct stat st;
				if(stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
					return c;
			}
			return "";
		}
	} // namespace

	I32 runTestSuite(I32 argc, char** argv, const TestSuiteSpec& spec) {
		U32 jobs = 1;
		List<String> cases;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			if(arg.rfind("-j", 0) == 0) {
				String num = arg.substr(2);
				if(num.empty() && i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
					num = argv[++i];
				if(!num.empty()) {
					long n = std::strtol(num.c_str(), nullptr, 10);
					if(n > 1)
						jobs = (U32)n;
				}
			} else {
				cases.push_back(arg);
			}
		}

		if(cases.empty()) {
			String dir = findDir(spec.dirCandidates);
			if(!dir.empty())
				collectCases(dir, spec.extension, cases);
			if(cases.empty()) {
				std::cerr << spec.tool << ": no case paths given and no case directory found\n";
				return 2;
			}
		}

		std::atomic<U32> passed{0};
		std::atomic<U32> failed{0};
		std::mutex ioMtx;
		List<String> failures;

		auto record = [&](const String& path, B32 ok, const String& err) {
			if(ok) {
				std::cout << "PASS  " << path << "\n";
				++passed;
			} else {
				std::cout << "FAIL  " << path << ": " << err << "\n";
				++failed;
				failures.push_back(path);
			}
		};

		if(jobs <= 1) {
			for(const String& path : cases) {
				String err;
				B32 ok = spec.run(path, err);
				record(path, ok, err);
			}
		} else {
			if(spec.prewarm)
				spec.prewarm();

			std::atomic<U32> next{0};
			auto worker = [&] {
				for(;;) {
					U32 i = next.fetch_add(1);
					if(i >= cases.size())
						break;
					const String& path = cases[i];
					String err;
					B32 ok = spec.run(path, err);
					std::lock_guard<std::mutex> lk(ioMtx);
					record(path, ok, err);
				}
			};

			if(jobs > cases.size())
				jobs = (U32)cases.size();
			List<std::thread> pool;
			for(U32 t = 0; t < jobs; ++t)
				pool.emplace_back(worker);
			for(std::thread& t : pool)
				t.join();
		}

		if(!failures.empty()) {
			std::sort(failures.begin(), failures.end());
			std::cout << "\n=== failures ===\n";
			for(const String& path : failures)
				std::cout << "FAIL  " << path << "\n";
		}

		std::cout << "\n" << passed.load() << " passed, " << failed.load() << " failed\n";
		return failed.load() == 0 ? 0 : 1;
	}
} // namespace rat
