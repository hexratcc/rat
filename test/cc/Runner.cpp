#include "Ast.h"
#include "Emit.h"
#include "Lexer.h"
#include "Parser.h"
#include "Preprocess.h"

#include "IR/TextParser.h"
#include "Support/StringUtil.h"

#include "rat.h"

using namespace rat;
using namespace rat::cc;

namespace {
	U32 oracleCounter = 0;

	const String& hostCC() {
		static String cc = [] {
			const char* env = std::getenv("CC");
			return String(env && *env ? env : "cc");
		}();
		return cc;
	}

	String tempDir() {
		const char* env = std::getenv("TMPDIR");
		String dir = env && *env ? env : "/tmp";
		if (!dir.empty() && dir.back() == '/')
			dir.pop_back();
		return dir;
	}

	String captureCmd(const String& cmd) {
		String out;
		FILE* p = popen(cmd.c_str(), "r");
		if (!p)
			return out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
			out.append(buf, n);
		pclose(p);
		return out;
	}

	const String& hostPredefs() {
		static String cache =
				captureCmd(hostCC() + " -std=c11 -dM -E -xc /dev/null 2>/dev/null") + "\n";
		return cache;
	}

	const List<String>& hostIncludeDirs() {
		static List<String> cache = [] {
			List<String> dirs;
			String v = captureCmd(hostCC() + " -E -v -xc /dev/null 2>&1");
			std::istringstream in(v);
			String line;
			B32 inList = false;
			while (std::getline(in, line)) {
				if (line.find("#include <...> search starts here:") != String::npos) {
					inList = true;
					continue;
				}
				if (line.find("End of search list.") != String::npos)
					break;
				if (!inList)
					continue;
				U32 b = 0;
				while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
					++b;
				U32 e = (U32)line.size();
				while (e > b && (line[e - 1] == '\r' || line[e - 1] == ' '))
					--e;
				if (e > b)
					dirs.push_back(line.substr(b, e - b));
			}
			return dirs;
		}();
		return cache;
	}

	struct Expectation {
		B32 hasValue = false;
		I32 value = 0;
		String passes = "fold,gvn";
		B32 hasOutput = false;
		String output;
	};

	String trim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
			++b;
		while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
			--e;
		return s.substr(b, e - b);
	}

	B32 commentBody(const String& line, String& body) {
		U32 b = 0;
		while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
			++b;
		if (b + 1 >= line.size() || line[b] != '/' || line[b + 1] != '/')
			return false;
		body = line.substr(b + 2);
		return true;
	}

	B32 parseDirectives(const String& src, Expectation& exp, String& err) {
		std::istringstream in(src);
		String line;
		B32 inOutput = false;
		while (std::getline(in, line)) {
			String body;
			if (!commentBody(line, body)) {
				inOutput = false;
				continue;
			}
			if (inOutput && body.rfind("|", 0) == 0) {
				String text = body.substr(1);
				if (!text.empty() && text[0] == ' ')
					text = text.substr(1);
				exp.output += text;
				exp.output += '\n';
				continue;
			}
			inOutput = false;
			String t = trim(body);
			auto colon = t.find(':');
			if (colon == String::npos)
				continue;
			String key = trim(t.substr(0, colon));
			String val = trim(t.substr(colon + 1));
			if (key == "expect") {
				exp.hasValue = true;
				exp.value = (I32)std::strtol(val.c_str(), nullptr, 0);
			} else if (key == "passes") {
				exp.passes = val;
			} else if (key == "output") {
				exp.hasOutput = true;
				inOutput = true;
			}
		}
		if (!exp.hasValue) {
			err = "missing '// expect:' directive";
			return false;
		}
		return true;
	}

	const ConstantNode* returnConstant(const Function& fn) {
		const ReturnNode* only = nullptr;
		for (const Node* n : fn) {
			const ReturnNode* r = dyn_cast<ReturnNode>(n);
			if (!r || !r->hasValue())
				continue;
			if (only)
				return nullptr;
			only = r;
		}
		return only ? dyn_cast<ConstantNode>(only->getValue()) : nullptr;
	}

	B32 runOracle(Module& mod, I32& out, String& capturedOut, String& err) {
		std::ostringstream base;
		base << tempDir() << "/ratcc_oracle_" << (long)getpid() << "_" << oracleCounter++;
		String cpath = base.str() + ".c";
		String xpath = base.str() + ".out";
		String rpath = base.str() + ".ret";

		std::ostringstream src;
		{
			PassManager pm;
			pm.add<RenameSymbolPass>("main", "__ratcc_user_main");
			pm.add<CEmitterPass>(src);
			pm.run(mod);
		}

		String wpath = base.str() + ".wrap.c";
		std::ostringstream wrap;
		wrap << "#include <stdio.h>\n";
		wrap << "extern int __ratcc_user_main(void);\n";
		wrap << "int main(void) {\n";
		wrap << "  int __r = (int)__ratcc_user_main();\n";
		wrap << "  FILE* __rf = fopen(\"" << rpath << "\", \"w\");\n";
		wrap << "  if (__rf) { fprintf(__rf, \"%d\", __r); fclose(__rf); }\n";
		wrap << "  return __r;\n";
		wrap << "}\n";

		{
			std::ofstream cf(cpath);
			if (!cf) {
				err = "oracle: cannot write temp source";
				return false;
			}
			cf << src.str();
			std::ofstream wf(wpath);
			if (!wf) {
				err = "oracle: cannot write wrapper source";
				return false;
			}
			wf << wrap.str();
		}

		std::ostringstream cmd;
		cmd << hostCC() << " -std=c11 -w -O0 -o '" << xpath << "' '" << cpath << "' '" << wpath
				<< "' -lm";
		I32 crc = std::system(cmd.str().c_str());
		if (crc != 0) {
			std::remove(cpath.c_str());
			std::remove(wpath.c_str());
			err = "oracle: C compilation failed";
			return false;
		}

		FILE* p = popen(xpath.c_str(), "r");
		if (!p) {
			std::remove(cpath.c_str());
			std::remove(wpath.c_str());
			std::remove(xpath.c_str());
			err = "oracle: cannot execute program";
			return false;
		}
		capturedOut.clear();
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
			capturedOut.append(buf, n);
		I32 status = pclose(p);

		B32 gotRet = false;
		{
			std::ifstream rf(rpath);
			if (rf) {
				String rv;
				std::getline(rf, rv);
				if (!rv.empty()) {
					out = (I32)std::strtol(rv.c_str(), nullptr, 10);
					gotRet = true;
				}
			}
		}
		if (!gotRet)
			out = WIFEXITED(status) ? (I32)WEXITSTATUS(status) : -1;

		std::remove(cpath.c_str());
		std::remove(wpath.c_str());
		std::remove(xpath.c_str());
		std::remove(rpath.c_str());
		return true;
	}

	B32 useX86Backend() {
		static B32 on = [] {
			const char* e = std::getenv("RATCC_X86");
			return (B32)(e && *e && String(e) != "0");
		}();
		return on;
	}

	B32 runOracleX86(Module& mod, I32& out, String& capturedOut, String& err) {
		std::ostringstream base;
		base << tempDir() << "/ratcc_x86_" << (long)getpid() << "_" << oracleCounter++;
		String opath = base.str() + ".o";
		String wpath = base.str() + ".wrap.c";
		String xpath = base.str() + ".out";
		String rpath = base.str() + ".ret";

		{
			std::ofstream of(opath, std::ios::binary);
			if (!of) {
				err = "x86: cannot write temp object";
				return false;
			}
			PassManager pm;
			pm.add<RenameSymbolPass>("main", "__ratcc_user_main");
			pm.add<X86EmitterPass>(of);
			pm.run(mod);
		}

		std::ostringstream wrap;
		wrap << "#include <stdio.h>\n";
		wrap << "extern int __ratcc_user_main(void);\n";
		wrap << "int main(void) {\n";
		wrap << "  int __r = (int)__ratcc_user_main();\n";
		wrap << "  FILE* __rf = fopen(\"" << rpath << "\", \"w\");\n";
		wrap << "  if (__rf) { fprintf(__rf, \"%d\", __r); fclose(__rf); }\n";
		wrap << "  return __r;\n";
		wrap << "}\n";
		{
			std::ofstream wf(wpath);
			if (!wf) {
				err = "x86: cannot write wrapper source";
				return false;
			}
			wf << wrap.str();
		}

		std::ostringstream cmd;
		cmd << hostCC() << " -w -O0 -no-pie -o '" << xpath << "' '" << wpath << "' '" << opath
				<< "' -lm";
		I32 crc = std::system(cmd.str().c_str());
		if (crc != 0) {
			std::remove(opath.c_str());
			std::remove(wpath.c_str());
			err = "x86: link failed";
			return false;
		}

		FILE* p = popen(xpath.c_str(), "r");
		if (!p) {
			std::remove(opath.c_str());
			std::remove(wpath.c_str());
			std::remove(xpath.c_str());
			err = "x86: cannot execute program";
			return false;
		}
		capturedOut.clear();
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
			capturedOut.append(buf, n);
		I32 status = pclose(p);

		B32 gotRet = false;
		{
			std::ifstream rf(rpath);
			if (rf) {
				String rv;
				std::getline(rf, rv);
				if (!rv.empty()) {
					out = (I32)std::strtol(rv.c_str(), nullptr, 10);
					gotRet = true;
				}
			}
		}
		if (!gotRet)
			out = WIFEXITED(status) ? (I32)WEXITSTATUS(status) : -1;

		std::remove(opath.c_str());
		std::remove(wpath.c_str());
		std::remove(xpath.c_str());
		std::remove(rpath.c_str());
		return true;
	}

	B32 runCase(const String& path, String& err) {
		std::ifstream f(path);
		if (!f) {
			err = "cannot open file";
			return false;
		}
		String source;
		if (!readAll(f, source)) {
			err = "failed to read file";
			return false;
		}

		Expectation exp;
		if (!parseDirectives(source, exp, err))
			return false;

		PpOptions pp;
		pp.includeDirs = hostIncludeDirs();
		String full = hostPredefs() + source;
		String pped;
		if (!preprocess(path, full, pp, pped, err))
			return false;
		source = pped;

		Lexer lex(source.data(), (U32)source.size(), path);
		Arena arena;
		Generic64 target;
		Parser parser(lex, arena, target);
		TransUnit* unit = parser.parseUnit();
		if (!unit) {
			err = parser.error();
			return false;
		}

		Module mod;
		mod.setTarget(&target);
		Emitter emitter(mod);
		if (!emitter.emit(*unit)) {
			err = emitter.error();
			return false;
		}

		if (!exp.passes.empty()) {
			PassManager pm;
			std::ostringstream sink;
			String perr;
			if (!buildPipeline(pm, exp.passes, sink, perr)) {
				err = "bad pass spec: " + perr;
				return false;
			}
			pm.run(mod);
		}

		Function* main = mod.getFunction("main");
		if (!main) {
			err = "no 'main' function";
			return false;
		}

		I32 got;
		String capturedOut;
		const ConstantNode* result = exp.hasOutput ? nullptr : returnConstant(*main);
		if (result) {
			got = (I32)result->getValue();
		} else if (useX86Backend()) {
			if (!runOracleX86(mod, got, capturedOut, err))
				return false;
		} else if (!runOracle(mod, got, capturedOut, err)) {
			return false;
		}

		if (got != exp.value) {
			std::ostringstream os;
			os << "expected " << exp.value << ", got " << got;
			err = os.str();
			return false;
		}

		if (exp.hasOutput && capturedOut != exp.output) {
			err = "stdout mismatch:\n--- expected ---\n" + exp.output + "\n--- got ---\n" + capturedOut +
						"\n---";
			return false;
		}
		return true;
	}

	String ratTrim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while (b < e && std::isspace((U8)s[b]))
			++b;
		while (e > b && std::isspace((U8)s[e - 1]))
			--e;
		return s.substr(b, e - b);
	}

	List<String> normalizeLines(const String& text) {
		List<String> out;
		std::istringstream ss(stripAnsi(text));
		String line;
		while (std::getline(ss, line)) {
			String t = ratTrim(line);
			if (!t.empty())
				out.push_back(t);
		}
		return out;
	}

	String emitToString(Module& m) {
		std::ostringstream os;
		PassManager pm;
		pm.add<TextEmitterPass>(os);
		pm.run(m);
		return os.str();
	}

	B32 canonicalIR(const String& text, String& out, String& err) {
		Generic64 target;
		Module m;
		m.setTarget(&target);
		std::ostringstream es;
		if (!parseText(text, m, es)) {
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
		while (std::getline(ss, line)) {
			String t = ratTrim(line);
			if (t.rfind("@name", 0) == 0) {
				tf.name = ratTrim(t.substr(5));
				section = 0;
			} else if (t.rfind("@passes", 0) == 0) {
				std::istringstream ps(t.substr(7));
				String p;
				while (ps >> p)
					tf.passes.push_back(p);
				section = 0;
			} else if (t == "@input") {
				section = 1;
			} else if (t == "@expect") {
				section = 2;
			} else if (!t.empty() && t[0] == '@') {
				err = "unknown directive: " + t;
				return false;
			} else if (section == 1) {
				tf.input += line + "\n";
			} else if (section == 2) {
				tf.expect += line + "\n";
			}
		}
		if (tf.input.empty()) {
			err = "missing @input section";
			return false;
		}
		if (tf.expect.empty()) {
			err = "missing @expect section";
			return false;
		}
		return true;
	}

	String formatRatDiff(const List<String>& expect, const List<String>& actual) {
		String s = "--- expected ---\n";
		for (const String& l : expect)
			s += "    " + l + "\n";
		s += "--- actual ---\n";
		for (const String& l : actual)
			s += "    " + l + "\n";
		return s;
	}

	B32 runRatCase(const String& path, String& err) {
		std::ifstream f(path);
		if (!f) {
			err = "cannot read file";
			return false;
		}
		String text;
		if (!readAll(f, text)) {
			err = "failed to read file";
			return false;
		}

		RatTestFile tf;
		if (!parseRatTestFile(text, tf, err))
			return false;

		Generic64 target;
		Module mod;
		mod.setTarget(&target);
		std::ostringstream perr;
		if (!parseText(tf.input, mod, perr)) {
			err = "input parse error\n    " + ratTrim(perr.str());
			return false;
		}

		std::ostringstream sink;
		PassManager pm;
		String spec;
		for (const String& p : tf.passes) {
			if (!spec.empty())
				spec += ',';
			spec += p;
		}
		String perr2;
		if (!buildPipeline(pm, spec, sink, perr2)) {
			err = perr2;
			return false;
		}
		pm.run(mod);

		String actualCanon, expectCanon, cerr;
		if (!canonicalIR(emitToString(mod), actualCanon, cerr)) {
			err = "cannot re-parse actual output\n    " + ratTrim(cerr);
			return false;
		}
		if (!canonicalIR(tf.expect, expectCanon, cerr)) {
			err = "@expect parse error\n    " + ratTrim(cerr);
			return false;
		}

		List<String> a = normalizeLines(actualCanon);
		List<String> e = normalizeLines(expectCanon);
		if (a != e) {
			err = formatRatDiff(e, a);
			return false;
		}
		return true;
	}
} // namespace

const I32 kCaseTimeoutSec = 20;

B32 runCaseForked(const String& path, String& err) {
	I32 fds[2];
	if (pipe(fds) != 0)
		return runCase(path, err); // fall back to in-process on pipe failure

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return runCase(path, err);
	}

	if (pid == 0) {
		setpgid(0, 0); // lead a new group so the parent can kill descendants too
		close(fds[0]);
		String cerr;
		B32 ok = runCase(path, cerr);
		if (!ok && !cerr.empty())
			(void)!write(fds[1], cerr.data(), cerr.size());
		close(fds[1]);
		_exit(ok ? 0 : 1);
	}

	setpgid(pid, pid); // race-free against the child's own setpgid
	close(fds[1]);

	String childErr;
	B32 timedOut = false;
	time_t deadline = time(nullptr) + kCaseTimeoutSec;
	for (;;) {
		time_t now = time(nullptr);
		if (now >= deadline) {
			timedOut = true;
			break;
		}
		struct pollfd pfd = {fds[0], POLLIN, 0};
		I32 pr = poll(&pfd, 1, (I32)((deadline - now) * 1000));
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0) {
			timedOut = true;
			break;
		}
		char buf[4096];
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n > 0)
			childErr.append(buf, (size_t)n);
		else
			break; // EOF (child closed the pipe) or read error
	}
	close(fds[0]);

	if (timedOut) {
		kill(-pid, SIGKILL);
		I32 dummy = 0;
		waitpid(pid, &dummy, 0);
		std::ostringstream os;
		os << "timeout (>" << kCaseTimeoutSec << "s)";
		err = os.str();
		return false;
	}

	I32 status = 0;
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		std::ostringstream os;
		os << "crashed (signal " << WTERMSIG(status) << ")";
		err = os.str();
		return false;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return true;
	err = childErr.empty() ? "failed" : childErr;
	return false;
}

static B32 hasSuffix(const String& s, const char* suffix) {
	String suf = suffix;
	return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static B32 runAnyCase(const String& path, String& err) {
	if (hasSuffix(path, ".rat"))
		return runRatCase(path, err);
	return runCaseForked(path, err);
}

static void collectCases(const String& dir, const char* ext, List<String>& out) {
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	List<String> subdirs;
	List<String> files;
	for (struct dirent* e; (e = readdir(d));) {
		String name = e->d_name;
		if (name == "." || name == "..")
			continue;
		String path = dir + "/" + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			subdirs.push_back(path);
		else if (hasSuffix(name, ext))
			files.push_back(path);
	}
	closedir(d);
	std::sort(files.begin(), files.end());
	std::sort(subdirs.begin(), subdirs.end());
	for (const String& f : files)
		out.push_back(f);
	for (const String& s : subdirs)
		collectCases(s, ext, out);
}

static String findDir(const char* const* candidates, U32 count) {
	for (U32 i = 0; i < count; ++i) {
		struct stat st;
		if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
			return candidates[i];
	}
	return "";
}

static String findCasesDir() {
	const char* candidates[] = {"cases", "test/cc/cases"};
	return findDir(candidates, 2);
}

static String findRatCasesDir() {
	const char* candidates[] = {"../cases", "test/cases"};
	return findDir(candidates, 2);
}

I32 main(I32 argc, char** argv) {
	U32 jobs = 1;
	List<String> cases;
	for (I32 i = 1; i < argc; ++i) {
		String arg = argv[i];
		if (arg.rfind("-j", 0) == 0) {
			String num = arg.substr(2);
			if (num.empty() && i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				num = argv[++i];
			if (!num.empty()) {
				long n = std::strtol(num.c_str(), nullptr, 10);
				if (n > 1)
					jobs = (U32)n;
			}
		} else {
			cases.push_back(arg);
		}
	}

	if (cases.empty()) {
		String ccDir = findCasesDir();
		if (!ccDir.empty())
			collectCases(ccDir, ".c", cases);
		String ratDir = findRatCasesDir();
		if (!ratDir.empty())
			collectCases(ratDir, ".rat", cases);
		if (cases.empty()) {
			std::cerr << "ratcc-test: no case paths given and no cases/ "
									 "directories found\n";
			return 2;
		}
	}

	std::atomic<U32> passed{0};
	std::atomic<U32> failed{0};
	std::mutex ioMtx;
	List<String> failures;

	auto record = [&](const String& path, B32 ok, const String& err) {
		if (ok) {
			std::cout << "PASS  " << path << "\n";
			++passed;
		} else {
			std::cout << "FAIL  " << path << ": " << err << "\n";
			++failed;
			failures.push_back(path);
		}
	};

	if (jobs <= 1) {
		for (const String& path : cases) {
			String err;
			B32 ok = runAnyCase(path, err);
			record(path, ok, err);
		}
	} else {
		(void)hostCC();
		(void)hostPredefs();
		(void)hostIncludeDirs();
		(void)useX86Backend();

		std::atomic<size_t> next{0};
		auto worker = [&] {
			for (;;) {
				size_t i = next.fetch_add(1);
				if (i >= cases.size())
					break;
				const String& path = cases[i];
				String err;
				B32 ok = runAnyCase(path, err);
				std::lock_guard<std::mutex> lk(ioMtx);
				record(path, ok, err);
			}
		};

		if (jobs > cases.size())
			jobs = (U32)cases.size();
		List<std::thread> pool;
		for (U32 t = 0; t < jobs; ++t)
			pool.emplace_back(worker);
		for (std::thread& t : pool)
			t.join();
	}

	if (!failures.empty()) {
		std::sort(failures.begin(), failures.end());
		std::cout << "\n=== failures ===\n";
		for (const String& path : failures)
			std::cout << "FAIL  " << path << "\n";
	}

	std::cout << "\n" << passed.load() << " passed, " << failed.load() << " failed\n";
	return failed.load() == 0 ? 0 : 1;
}
