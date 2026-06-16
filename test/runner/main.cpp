// each .rat fixture is a single file describing one transform check:
//   @name   <human-readable name>   (optional)
//   @passes <p1> <p2> ...           (pass names, in order; may be empty)
//   @input                          (textual IR fed to the pipeline)
//   @expect                         (textual IR the pipeline must yield)
// the runner parses @input, runs the named passes, then compares the emitted
// IR against @expect

#include "CodeGen/Target.h"
#include "IR/Module.h"
#include "IR/TextParser.h"
#include "Pass/Emit/CEmitter.h"
#include "Pass/Emit/GraphEmitter.h"
#include "Pass/Emit/TextEmitter.h"
#include "Pass/Opt/Fold.h"
#include "Pass/Opt/GVN.h"
#include "Pass/Opt/MemoryOpt.h"
#include "Pass/Opt/SimplifyCFG.h"
#include "Pass/PassManager.h"
#include "Pass/Verify.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace rat;

namespace detail {
	String stripAnsi(const String& s) {
		String out;
		for (U32 i = 0; i < s.size();) {
			if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
				i += 2;
				while (i < s.size() && s[i] != 'm')
					++i;
				if (i < s.size())
					++i;
			} else {
				out.push_back(s[i++]);
			}
		}
		return out;
	}

	String trim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while (b < e && std::isspace((U8)s[b]))
			++b;
		while (e > b && std::isspace((U8)s[e - 1]))
			--e;
		return s.substr(b, e - b);
	}

	List<String> normalize(const String& text) {
		List<String> out;
		std::istringstream ss(stripAnsi(text));
		String line;
		while (std::getline(ss, line)) {
			String t = trim(line);
			if (!t.empty())
				out.push_back(t);
		}
		return out;
	}

	String emitToString(const Module& m) {
		std::ostringstream os;
		emitText(m, os);
		return os.str();
	}

	B32 canonical(const String& text, String& out, String& err) {
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

	B32 addPass(PassManager& pm, const String& name, std::ostream& sink) {
		if (name == "fold")
			pm.add<FoldPass>();
		else if (name == "gvn")
			pm.add<GVNPass>();
		else if (name == "simplifycfg")
			pm.add<SimplifyCFGPass>();
		else if (name == "memoryopt")
			pm.add<MemoryOptPass>();
		else if (name == "verify")
			pm.add<VerifyPass>(sink);
		else if (name == "text-emitter")
			pm.add<TextEmitterPass>(sink);
		else if (name == "graph-emitter")
			pm.add<GraphEmitterPass>(sink);
		else if (name == "c-emitter")
			pm.add<CEmitterPass>(sink);
		else
			return false;
		return true;
	}

	struct TestFile {
		String name;
		List<String> passes;
		String input;
		String expect;
	};

	B32 readFile(const String& path, String& out) {
		std::ifstream f(path);
		if (!f)
			return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	B32 parseTestFile(const String& text, TestFile& tf, String& err) {
		std::istringstream ss(text);
		String line;
		I32 section = 0; // 0 none, 1 input, 2 expect
		while (std::getline(ss, line)) {
			String t = trim(line);
			if (t.rfind("@name", 0) == 0) {
				tf.name = trim(t.substr(5));
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

	void printDiff(const List<String>& expect, const List<String>& actual) {
		std::cout << "    --- expected ---\n";
		for (const String& l : expect)
			std::cout << "    " << l << "\n";
		std::cout << "    --- actual ---\n";
		for (const String& l : actual)
			std::cout << "    " << l << "\n";
	}

	B32 runTest(const String& path) {
		String text;
		if (!readFile(path, text)) {
			std::cout << "[FAIL] " << path << ": cannot read file\n";
			return false;
		}
		TestFile tf;
		String err;
		if (!parseTestFile(text, tf, err)) {
			std::cout << "[FAIL] " << path << ": " << err << "\n";
			return false;
		}
		String label = tf.name.empty() ? path : (tf.name + " (" + path + ")");

		Generic64 target;
		Module mod;
		mod.setTarget(&target);
		std::ostringstream perr;
		if (!parseText(tf.input, mod, perr)) {
			std::cout << "[FAIL] " << label << ": input parse error\n    "
								<< trim(perr.str()) << "\n";
			return false;
		}

		std::ostringstream sink;
		PassManager pm;
		for (const String& p : tf.passes) {
			if (!addPass(pm, p, sink)) {
				std::cout << "[FAIL] " << label << ": unknown pass '" << p << "'\n";
				return false;
			}
		}
		pm.run(mod);

		String actualCanon, expectCanon, cerr;
		if (!canonical(emitToString(mod), actualCanon, cerr)) {
			std::cout << "[FAIL] " << label << ": cannot re-parse actual output\n    "
								<< trim(cerr) << "\n";
			return false;
		}
		if (!canonical(tf.expect, expectCanon, cerr)) {
			std::cout << "[FAIL] " << label << ": @expect parse error\n    "
								<< trim(cerr) << "\n";
			return false;
		}

		List<String> a = normalize(actualCanon);
		List<String> e = normalize(expectCanon);
		if (a != e) {
			std::cout << "[FAIL] " << label << "\n";
			printDiff(e, a);
			return false;
		}
		std::cout << "[PASS] " << label << "\n";
		return true;
	}
} // namespace detail
using namespace ::detail;

I32 main(I32 argc, char** argv) {
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " <test.rat> [more.rat ...]\n";
		return 2;
	}
	U32 passed = 0, failed = 0;
	for (I32 i = 1; i < argc; ++i) {
		if (runTest(argv[i]))
			++passed;
		else
			++failed;
	}
	std::cout << "\n" << passed << " passed, " << failed << " failed\n";
	return failed ? 1 : 0;
}
