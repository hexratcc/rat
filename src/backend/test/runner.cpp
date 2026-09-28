#include "ir/text_parser.h"

#include "string.h"
#include "test_harness.h"
#include <fstream>
#include <sstream>

#include "rat.h"

using namespace rat;

namespace detail {
	struct RatTestFile {
		String name;
		List<String> passes;
		String input;
		String expect;
	};

	List<String> normalizeLines(const String& text) {
		List<String> out;
		std::istringstream ss(stripAnsi(text));
		String line;
		while(std::getline(ss, line)) {
			String t = trim(line);
			if(!t.empty())
				out.push_back(t);
		}
		return out;
	}

	String emitToString(Module& m) {
		std::ostringstream os;
		Generic64 target;
		PassManager pm(target);
		pm.add<TextEmitterPass>(os);
		pm.run(m);
		return os.str();
	}

	B32 canonicalIR(const String& text, String& out, String& err) {
		Module m;
		std::ostringstream es;
		if(!parseText(text, m, es)) {
			err = es.str();
			return false;
		}
		out = emitToString(m);
		return true;
	}

	B32 parseRatTestFile(const String& text, RatTestFile& tf, String& err) {
		std::istringstream ss(text);
		String line;
		I32 section = 0; // 0 none, 1 input, 2 expect
		while(std::getline(ss, line)) {
			String t = trim(line);
			if(t.rfind("@name", 0) == 0) {
				tf.name = trim(t.substr(5));
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

	B32 readFile(const String& path, String& text, String& err) {
		std::ifstream f(path);
		if(!f) {
			err = "cannot read file";
			return false;
		}
		if(!readAll(f, text)) {
			err = "failed to read file";
			return false;
		}
		return true;
	}

	B32 runRatPasses(Module& mod, const List<String>& passes, String& err) {
		Generic64 target;
		PassManager pm(target);
		std::ostringstream sink;
		for(const String& p : passes) {
			UniquePtr<Pass> pass = createPass(p, sink);
			if(!pass) {
				err = "unknown pass: " + p;
				return false;
			}
			pm.add(std::move(pass));
		}
		pm.run(mod);
		String diags = trim(sink.str());
		if(!diags.empty()) {
			err = "pass diagnostics\n    " + diags;
			return false;
		}
		return true;
	}

	B32 matchesExpect(Module& mod, const String& expect, String& err) {
		String actualCanon;
		String expectCanon;
		String cerr;
		if(!canonicalIR(emitToString(mod), actualCanon, cerr)) {
			err = "cannot re-parse actual output\n    " + trim(cerr);
			return false;
		}
		if(!canonicalIR(expect, expectCanon, cerr)) {
			err = "@expect parse error\n    " + trim(cerr);
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

	B32 runRatCase(const String& path, String& err) {
		String text;
		RatTestFile tf;
		if(!readFile(path, text, err) || !parseRatTestFile(text, tf, err))
			return false;

		Module mod;
		std::ostringstream perr;
		if(!parseText(tf.input, mod, perr)) {
			err = "input parse error\n    " + trim(perr.str());
			return false;
		}
		return runRatPasses(mod, tf.passes, err) && matchesExpect(mod, tf.expect, err);
	}
} // namespace detail

I32 main(I32 argc, char** argv) {
	TestSuiteSpec spec;
	spec.tool = "rat-test";
	spec.extension = ".rat";
	spec.dirCandidates = {"src/backend/test", "test"};
	spec.run = ::detail::runRatCase;
	return runTestSuite(argc, argv, spec);
}
