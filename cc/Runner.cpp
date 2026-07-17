#include "Compile.h"

#include "Emit/Emit.h"
#include "Lex/Lexer.h"
#include "Lex/Preprocess.h"
#include "Parse/Parser.h"
#include <atomic>
#include <fstream>
#if defined(_WIN32)
#include <process.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Support/StringUtil.h"
#include "Support/TestHarness.h"

#include "rat.h"

using namespace rat;
using namespace rat::cc;

namespace {
	constexpr U32 kReadBufSize = 4096;

	std::atomic<U32> oracleCounter{0};

	String tempDir() {
		const char* env = std::getenv("TMPDIR");
#if defined(_WIN32)
		if(!env || !*env)
			env = std::getenv("TEMP");
		if(!env || !*env)
			env = std::getenv("TMP");
		String dir = env && *env ? env : ".";
#else
		String dir = env && *env ? env : "/tmp";
#endif
		while(!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
			dir.pop_back();
		for(char& c : dir)
			if(c == '\\')
				c = '/';
		return dir;
	}

	B32 targetIsWindows() { return hostTargetTriple().isWindows(); }

	String defaultPasses() {
		String spec;
		for(const String& p : defaultOptPipeline())
			spec += (spec.empty() ? "" : ",") + p;
		return spec;
	}

	struct Expectation {
		B32 hasValue = false;
		I32 value = 0;
		String passes = defaultPasses();
		B32 hasOutput = false;
		String output;
		B32 skip = false;
	};

	String trim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
			++b;
		while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
			--e;
		return s.substr(b, e - b);
	}

	B32 commentBody(const String& line, String& body) {
		U32 b = 0;
		while(b < line.size() && (line[b] == ' ' || line[b] == '\t'))
			++b;
		if(b + 1 >= line.size() || line[b] != '/' || line[b + 1] != '/')
			return false;
		body = line.substr(b + 2);
		return true;
	}

	B32 parseDirectives(const String& src, Expectation& exp, String& err) {
		std::istringstream in(src);
		String line;
		B32 inOutput = false;
		while(std::getline(in, line)) {
			String body;
			if(!commentBody(line, body)) {
				inOutput = false;
				continue;
			}
			if(inOutput && body.rfind("|", 0) == 0) {
				String text = body.substr(1);
				if(!text.empty() && text[0] == ' ')
					text = text.substr(1);
				exp.output += text;
				exp.output += '\n';
				continue;
			}
			inOutput = false;
			String t = trim(body);
			auto colon = t.find(':');
			if(colon == String::npos)
				continue;
			String key = trim(t.substr(0, colon));
			String val = trim(t.substr(colon + 1));
			if(key == "expect") {
				exp.hasValue = true;
				exp.value = (I32)std::strtol(val.c_str(), nullptr, 0);
			} else if(key == "passes") {
				exp.passes = val;
			} else if(key == "output") {
				exp.hasOutput = true;
				inOutput = true;
			} else if(key == "skip-target") {
				TargetTriple t;
				String terr;
				B32 match = TargetTriple::parse(val, t, terr) ? t.os == hostTargetTriple().os
																											: val == String(hostTargetTriple().osName());
				if(match)
					exp.skip = true;
			}
		}
		if(!exp.hasValue) {
			err = "missing '// expect:' directive";
			return false;
		}
		return true;
	}

	const ConstantNode* returnConstant(const Function& fn) {
		const ReturnNode* only = nullptr;
		for(const Node* n : fn) {
			const ReturnNode* r = dyn_cast<ReturnNode>(n);
			if(!r || !r->hasValue())
				continue;
			if(only)
				return nullptr;
			only = r;
		}
		return only ? dyn_cast<ConstantNode>(only->getValue()) : nullptr;
	}

	B32 useX86Backend() {
		static B32 on = [] {
			const char* e = std::getenv("RATCC_X86");
			return (B32)(!e || !*e || String(e) != "0");
		}();
		return on;
	}

	B32 useGraphRegAlloc() {
		static B32 on = [] {
			const char* e = std::getenv("RATCC_REGALLOC");
			return (B32)(e && String(e) == "graph");
		}();
		return on;
	}

	struct Artifact {
		String path;
		String compileArgs;
	};

	B32 runBackend(const char* tag,
								 const Delegate<B32(const String& base, Artifact&, String&)>& makeArtifact,
								 I32& out,
								 String& capturedOut,
								 String& err) {
		std::ostringstream basess;
#if defined(_WIN32)
		long pid = (long)_getpid();
#else
		long pid = (long)getpid();
#endif
		basess << tempDir() << "/ratcc_" << tag << "_" << pid << "_" << oracleCounter++;
		String base = basess.str();
		String wpath = base + ".wrap.c";
		String xpath = base + (targetIsWindows() ? ".exe" : ".out");
		String rpath = base + ".ret";

		Artifact art;
		if(!makeArtifact(base, art, err))
			return false;

		auto cleanup = [&] {
			std::remove(art.path.c_str());
			std::remove(wpath.c_str());
			std::remove(xpath.c_str());
			std::remove(rpath.c_str());
		};

		{
			std::ofstream wf(wpath);
			if(!wf) {
				cleanup();
				err = String(tag) + ": cannot write wrapper source";
				return false;
			}
			wf << "#include <stdio.h>\n"
				 << "#ifdef _WIN32\n"
				 << "#include <fcntl.h>\n"
				 << "#include <io.h>\n"
				 << "#endif\n"
				 << "extern int __ratcc_user_main();\n"
				 << "static char* __ratcc_argv[] = { (char*)\"a.out\", 0 };\n"
				 << "int main(void) {\n"
				 << "#ifdef _WIN32\n"
				 << "  _setmode(_fileno(stdout), _O_BINARY);\n"
				 << "#endif\n"
				 << "  int __r = (int)__ratcc_user_main(1, __ratcc_argv);\n"
				 << "  fflush(stdout);\n"
				 << "  FILE* __rf = fopen(\"" << rpath << "\", \"w\");\n"
				 << "  if (__rf) { fprintf(__rf, \"%d\", __r); fclose(__rf); }\n"
				 << "  return __r;\n"
				 << "}\n";
		}

		String cmd = hostCC() + " -w -O0 " + art.compileArgs + " \"" + wpath + "\" -o \"" + xpath +
								 "\" -lm";
		if(std::system(cmd.c_str()) != 0) {
			cleanup();
			err = String(tag) + ": compilation/link failed";
			return false;
		}

		String runCmd = "\"" + xpath + "\" 2>" + nullDevice();
		FILE* p = shellOpen(runCmd.c_str());
		if(!p) {
			cleanup();
			err = String(tag) + ": cannot execute program";
			return false;
		}
		capturedOut.clear();
		char buf[kReadBufSize];
		U64 n;
		while((n = fread(buf, 1, sizeof(buf), p)) > 0)
			capturedOut.append(buf, n);
		I32 status = shellClose(p);

		std::ifstream rf(rpath);
		String rv;
		if(rf && std::getline(rf, rv) && !rv.empty())
			out = (I32)std::strtol(rv.c_str(), nullptr, 10);
		else
#if defined(_WIN32)
			out = status;
#else
			out = WIFEXITED(status) ? (I32)WEXITSTATUS(status) : -1;
#endif

		if(!std::getenv("RATCC_KEEP"))
			cleanup();
		return true;
	}

	B32 runOracle(Module& mod, I32& out, String& capturedOut, String& err) {
		CompileOptions copt;
		copt.renameMain = "__ratcc_user_main";
		if(useX86Backend()) {
			copt.backend = Backend::X86;
			copt.regAlloc = useGraphRegAlloc() ? RegAlloc::Graph : RegAlloc::Linear;
			auto make = [&](const String& base, Artifact& art, String& e) -> B32 {
				art.path = base + ".o";
				std::ofstream of(art.path, std::ios::binary);
				if(!of) {
					e = "x86: cannot write temp object";
					return false;
				}
				X86Target target(hostTargetTriple());
				compileModule(mod, target, copt, of);
				art.compileArgs = targetIsWindows() ? "'" + art.path + "'" : "-no-pie '" + art.path + "'";
				return true;
			};
			return runBackend("x86", make, out, capturedOut, err);
		}
		copt.backend = Backend::C;
		auto make = [&](const String& base, Artifact& art, String& e) -> B32 {
			art.path = base + ".c";
			std::ofstream cf(art.path);
			if(!cf) {
				e = "oracle: cannot write temp source";
				return false;
			}
			Generic64 target;
			compileModule(mod, target, copt, cf);
			art.compileArgs = "-std=c11 '" + art.path + "'";
			return true;
		};
		return runBackend("oracle", make, out, capturedOut, err);
	}

	B32 runCase(const String& path, String& err) {
		std::ifstream f(path);
		if(!f) {
			err = "cannot open file";
			return false;
		}
		String source;
		if(!readAll(f, source)) {
			err = "failed to read file";
			return false;
		}

		Expectation exp;
		if(!parseDirectives(source, exp, err))
			return false;
		if(exp.skip)
			return true;

		PpOptions pp;
		pp.includeDirs = hostIncludeDirs();
		String full = hostPredefs() + "#line 1 \"" + path + "\"\n" + source;
		String pped;
		if(!preprocess(path, full, pp, pped, err))
			return false;
		source = pped;

		Lexer lex(source.data(), (U32)source.size(), path);
		Arena arena;
		Generic64 target;
		Parser parser(lex,
									arena,
									target.getPointerSizeInBytes(),
									targetLongBits(hostTargetTriple()),
									targetIsWindows());
		TransUnit* unit = parser.parseUnit();
		if(!unit) {
			err = parser.error();
			return false;
		}

		Module mod;
		Emitter emitter(mod, target.getPointerSizeInBytes(), targetIsWindows());
		if(!emitter.emit(*unit)) {
			err = emitter.error();
			return false;
		}

		if(!exp.passes.empty()) {
			std::ostringstream sink;
			String perr;
			PassManager pm(target);
			if(!buildPipeline(pm, exp.passes, sink, perr)) {
				err = "bad pass spec: " + perr;
				return false;
			}
			pm.run(mod);
		}

		Function* main = mod.getFunction("main");
		if(!main) {
			err = "no 'main' function";
			return false;
		}

		I32 got;
		String capturedOut;
		const ConstantNode* result = exp.hasOutput ? nullptr : returnConstant(*main);
		if(result)
			got = (I32)result->getValue();
		else if(!runOracle(mod, got, capturedOut, err))
			return false;

		if(got != exp.value) {
			std::ostringstream os;
			os << "expected " << exp.value << ", got " << got;
			err = os.str();
			return false;
		}

		if(exp.hasOutput && capturedOut != exp.output) {
			err = "stdout mismatch:\n--- expected ---\n" + exp.output + "\n--- got ---\n" + capturedOut +
						"\n---";
			return false;
		}
		return true;
	}
} // namespace

const I32 kCaseTimeoutSec = 20;

#if defined(_WIN32)
B32 runCaseForked(const String& path, String& err) { return runCase(path, err); }
#else
B32 runCaseForked(const String& path, String& err) {
	I32 fds[2];
	if(pipe(fds) != 0)
		return runCase(path, err); // fall back to in-process on pipe failure

	pid_t pid = fork();
	if(pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return runCase(path, err);
	}

	if(pid == 0) {
		setpgid(0, 0); // lead a new group so the parent can kill descendants too
		close(fds[0]);
		String cerr;
		B32 ok = runCase(path, cerr);
		if(!ok && !cerr.empty()) {
			I64 ignored = write(fds[1], cerr.data(), cerr.size());
			(void)ignored;
		}
		close(fds[1]);
		_exit(ok ? 0 : 1);
	}

	setpgid(pid, pid); // race-free against the child's own setpgid
	close(fds[1]);

	String childErr;
	B32 timedOut = false;
	time_t deadline = time(nullptr) + kCaseTimeoutSec;
	for(;;) {
		time_t now = time(nullptr);
		if(now >= deadline) {
			timedOut = true;
			break;
		}
		struct pollfd pfd = {fds[0], POLLIN, 0};
		I32 pr = poll(&pfd, 1, (I32)((deadline - now) * 1000));
		if(pr < 0) {
			if(errno == EINTR)
				continue;
			break;
		}
		if(pr == 0) {
			timedOut = true;
			break;
		}
		char buf[kReadBufSize];
		I64 n = read(fds[0], buf, sizeof(buf));
		if(n > 0)
			childErr.append(buf, (U64)n);
		else
			break; // EOF (child closed the pipe) or read error
	}
	close(fds[0]);

	if(timedOut) {
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
	if(WIFSIGNALED(status)) {
		std::ostringstream os;
		os << "crashed (signal " << WTERMSIG(status) << ")";
		err = os.str();
		return false;
	}
	if(WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return true;
	err = childErr.empty() ? "failed" : childErr;
	return false;
}
#endif

I32 main(I32 argc, char** argv) {
	TestSuiteSpec spec;
	spec.tool = "ratcc-test";
	spec.extension = ".c";
	spec.dirCandidates = {"cc/test", "test"};
	spec.run = [](const String& path, String& err) { return runCaseForked(path, err); };
	// warm the lazily-initialized host/config caches before threads spawn so
	// their first access does not race.
	spec.prewarm = [] {
		(void)hostCC();
		(void)hostPredefs();
		(void)hostIncludeDirs();
		(void)useX86Backend();
		(void)useGraphRegAlloc();
	};
	return runTestSuite(argc, argv, spec);
}
