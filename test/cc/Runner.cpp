#include "Ast.h"
#include "Emit.h"
#include "Lexer.h"
#include "Parser.h"

#include "rat.h"

using namespace rat;
using namespace rat::cc;

namespace {
	struct Expectation {
		B32 hasValue = false;
		I32 value = 0;
		String passes = "fold,gvn";
	};

	B32 readAll(std::istream& in, String& out) {
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return (B32)!in.bad();
	}

	String trim(const String& s) {
		U32 b = 0, e = (U32)s.size();
		while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
			++b;
		while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
			--e;
		return s.substr(b, e - b);
	}

	B32 parseDirectives(const String& src, Expectation& exp, String& err) {
		std::istringstream in(src);
		String line;
		while (std::getline(in, line)) {
			String t = trim(line);
			if (t.rfind("//", 0) != 0)
				continue;
			String body = trim(t.substr(2));
			auto colon = body.find(':');
			if (colon == String::npos)
				continue;
			String key = trim(body.substr(0, colon));
			String val = trim(body.substr(colon + 1));
			if (key == "expect") {
				exp.hasValue = true;
				exp.value = (I32)std::strtol(val.c_str(), nullptr, 0);
			} else if (key == "passes") {
				exp.passes = val;
			}
		}
		if (!exp.hasValue) {
			err = "missing '// expect:' directive";
			return false;
		}
		return true;
	}

	Function* findFunction(Module& mod, const String& name) {
		for (Function* fn : mod)
			if (fn->getName() == name)
				return fn;
		return nullptr;
	}

	const ConstantNode* returnConstant(const Function& fn) {
		for (const Node* n : fn) {
			const ReturnNode* r = dyn_cast<ReturnNode>(n);
			if (r && r->hasValue())
				return dyn_cast<ConstantNode>(r->getValue());
		}
		return nullptr;
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

		Lexer lex(source.data(), (U32)source.size(), path);
		Arena arena;
		Parser parser(lex, arena);
		TransUnit* unit = parser.parseUnit();
		if (!unit) {
			err = parser.error();
			return false;
		}

		Generic64 target;
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

		Function* main = findFunction(mod, "main");
		if (!main) {
			err = "no 'main' function";
			return false;
		}

		const ConstantNode* result = returnConstant(*main);
		if (!result) {
			err = "main did not fold to a constant return";
			return false;
		}

		I32 got = (I32)result->getValue();
		if (got != exp.value) {
			std::ostringstream os;
			os << "expected " << exp.value << ", got " << got;
			err = os.str();
			return false;
		}
		return true;
	}
} // namespace

I32 main(I32 argc, char** argv) {
	if (argc < 2) {
		std::cerr << "usage: ratcc-test <case.c> [case.c ...]\n";
		return 2;
	}

	U32 passed = 0;
	U32 failed = 0;
	for (I32 i = 1; i < argc; ++i) {
		String path = argv[i];
		String err;
		if (runCase(path, err)) {
			std::cout << "PASS  " << path << "\n";
			++passed;
		} else {
			std::cout << "FAIL  " << path << ": " << err << "\n";
			++failed;
		}
	}

	std::cout << "\n" << passed << " passed, " << failed << " failed\n";
	return failed == 0 ? 0 : 1;
}
