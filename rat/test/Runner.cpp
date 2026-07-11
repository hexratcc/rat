#include "IR/TextParser.h"

#include "Support/StringUtil.h"
#include "Support/TestHarness.h"
#include <fstream>

#include "rat.h"

using namespace rat;

namespace {
	template <class AddPasses>
	void runPasses(Module& mod, const TargetInfo& target, AddPasses&& add) {
		PassManager pm(target);
		add(pm);
		pm.run(mod);
	}

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
			err = "input parse error\n    " + trim(perr.str());
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
			err = "cannot re-parse actual output\n    " + trim(cerr);
			return false;
		}
		if(!canonicalIR(tf.expect, expectCanon, cerr)) {
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

} // namespace

I32 main(I32 argc, char** argv) {
	TestSuiteSpec spec;
	spec.tool = "rat-test";
	spec.extension = ".rat";
	spec.dirCandidates = {"rat/test", "test"};
	spec.run = [](const String& path, String& err) { return runRatCase(path, err); };
	return runTestSuite(argc, argv, spec);
}
