#include "IR/TextParser.h"
#include "Support/StringUtil.h"

#include "rat.h"

using namespace rat;

namespace {
	template <class AddPasses>
	void runPasses(Module& mod, const TargetInfo& target, AddPasses&& add) {
		PassManager pm(target);
		add(pm);
		pm.run(mod);
	}

	String ratTrim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while(b < e && std::isspace((U8)s[b]))
			++b;
		while(e > b && std::isspace((U8)s[e - 1]))
			--e;
		return s.substr(b, e - b);
	}

	List<String> normalizeLines(const String& text) {
		List<String> out;
		std::istringstream ss(stripAnsi(text));
		String line;
		while(std::getline(ss, line)) {
			String t = ratTrim(line);
			if(!t.empty())
				out.push_back(t);
		}
		return out;
	}

	String emitToString(Module& m) {
		std::ostringstream os;
		Generic64 target;
		runPasses(m, target, [&](PassManager& pm) { pm.add<TextEmitterPass>(os); });
		return os.str();
	}

	B32 canonicalIR(const String& text, String& out, String& err) {
		Generic64 target;
		Module m;
		m.setTarget(&target);
		std::ostringstream es;
		if(!parseText(text, m, es)) {
			err = es.str();
			return false;
		}
		out = emitToString(m);
		return true;
	}

	struct RatTestFile {
		String name;
		List<String> passes;
		String input;
		String expect;
	};

	B32 parseRatTestFile(const String& text, RatTestFile& tf, String& err) {
		std::istringstream ss(text);
		String line;
		I32 section = 0; // 0 none, 1 input, 2 expect
		while(std::getline(ss, line)) {
			String t = ratTrim(line);
			if(t.rfind("@name", 0) == 0) {
				tf.name = ratTrim(t.substr(5));
				section = 0;
			} else if(t.rfind("@passes", 0) == 0) {
				std::istringstream ps(t.substr(7));
				String p;
				while(ps >> p)
					tf.passes.push_back(p);
				section = 0;
			} else if(t == "@input") {
				section = 1;
			} else if(t == "@expect") {
				section = 2;
			} else if(!t.empty() && t[0] == '@') {
				err = "unknown directive: " + t;
				return false;
			} else if(section == 1) {
				tf.input += line + "\n";
			} else if(section == 2) {
				tf.expect += line + "\n";
			}
		}
		if(tf.input.empty()) {
			err = "missing @input section";
			return false;
		}
		if(tf.expect.empty()) {
			err = "missing @expect section";
			return false;
		}
		return true;
	}

	String formatRatDiff(const List<String>& expect, const List<String>& actual) {
		String s = "--- expected ---\n";
		for(const String& l : expect)
			s += "    " + l + "\n";
		s += "--- actual ---\n";
		for(const String& l : actual)
			s += "    " + l + "\n";
		return s;
	}

	B32 runRatCase(const String& path, String& err) {
		std::ifstream f(path);
		if(!f) {
			err = "cannot read file";
			return false;
		}
		String text;
		if(!readAll(f, text)) {
			err = "failed to read file";
			return false;
		}

		RatTestFile tf;
		if(!parseRatTestFile(text, tf, err))
			return false;

		Generic64 target;
		Module mod;
		mod.setTarget(&target);
		std::ostringstream perr;
		if(!parseText(tf.input, mod, perr)) {
			err = "input parse error\n    " + ratTrim(perr.str());
			return false;
		}

		String spec;
		for(const String& p : tf.passes) {
			if(!spec.empty())
				spec += ',';
			spec += p;
		}
		std::ostringstream sink;
		String perr2;
		B32 ok = true;
		runPasses(mod, target, [&](PassManager& pm) { ok = buildPipeline(pm, spec, sink, perr2); });
		if(!ok) {
			err = perr2;
			return false;
		}

		String actualCanon, expectCanon, cerr;
		if(!canonicalIR(emitToString(mod), actualCanon, cerr)) {
			err = "cannot re-parse actual output\n    " + ratTrim(cerr);
			return false;
		}
		if(!canonicalIR(tf.expect, expectCanon, cerr)) {
			err = "@expect parse error\n    " + ratTrim(cerr);
			return false;
		}

		List<String> a = normalizeLines(actualCanon);
		List<String> e = normalizeLines(expectCanon);
		if(a != e) {
			err = formatRatDiff(e, a);
			return false;
		}
		return true;
	}

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

	String findDir(const char* const* candidates, U32 count) {
		for(U32 i = 0; i < count; ++i) {
			struct stat st;
			if(stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
				return candidates[i];
		}
		return "";
	}

	String findRatCasesDir() {
		const char* candidates[] = {"rat/test", "test"};
		return findDir(candidates, 2);
	}
} // namespace

I32 main(I32 argc, char** argv) {
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
		String ratDir = findRatCasesDir();
		if(!ratDir.empty())
			collectCases(ratDir, ".rat", cases);
		if(cases.empty()) {
			std::cerr << "rat-test: no case paths given and no rat/test/ directory found\n";
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
			B32 ok = runRatCase(path, err);
			record(path, ok, err);
		}
	} else {
		std::atomic<size_t> next{0};
		auto worker = [&] {
			for(;;) {
				size_t i = next.fetch_add(1);
				if(i >= cases.size())
					break;
				const String& path = cases[i];
				String err;
				B32 ok = runRatCase(path, err);
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
