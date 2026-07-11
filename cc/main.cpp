#include "Compile.h"

#include <fstream>
#include "Emit.h"
#include "Lexer.h"
#include "Parser.h"
#include "Preprocess.h"

#include "Support/StringUtil.h"
#include "rat.h"

using namespace rat;
using namespace rat::cc;

namespace {
	const char* kVersion = "ratcc 0.1";

	enum struct Emit { Tok, Ast, C, X86 };

	struct Options {
		String input;							// root input file ("" => stdin)
		String output = "a.out";	// -o base; x86 object lands here
		List<Emit> emits;					// requested -emit kinds (in order)
		U32 optLevel = 0;					// -O0 / -O1
		List<String> extraPasses; // individual -f<pass> requests (in order)
		B32 timePasses = false;
		B32 preprocessOnly = false; // -E
		PpOptions pp;
	};

	List<UniquePtr<Pass>> buildOptPasses(const Options& opt) {
		std::ostringstream sink;
		List<UniquePtr<Pass>> passes;
		if(opt.optLevel >= 1)
			passes = defaultOptPasses();
		for(const String& name : opt.extraPasses)
			passes.push_back(passRegistry().create(name, sink));
		return passes;
	}

	B32 isOptPass(const String& name) {
		static const Set<String> kOptPasses = {
				"fold", "gvn", "sccp", "simplifycfg", "memoryopt", "inline"};
		return kOptPasses.count(name) != 0;
	}

	B32 parseEmit(const String& spec, List<Emit>& out, String& err) {
		auto add = [&](const String& k) -> B32 {
			if(k == "tok")
				out.push_back(Emit::Tok);
			else if(k == "ast")
				out.push_back(Emit::Ast);
			else if(k == "c")
				out.push_back(Emit::C);
			else if(k == "x86")
				out.push_back(Emit::X86);
			else if(!k.empty()) {
				err = "unknown -emit kind '" + k + "'";
				return false;
			}
			return true;
		};
		String tok;
		for(C8 ch : spec) {
			if(ch == ',') {
				if(!add(tok))
					return false;
				tok.clear();
			} else {
				tok.push_back(ch);
			}
		}
		return add(tok);
	}

	String baseName(const String& output) {
		U32 slash = (U32)output.rfind('/');
		U32 dot = (U32)output.rfind('.');
		if(dot != (U32)String::npos && (slash == (U32)String::npos || dot > slash))
			return output.substr(0, dot);
		return output;
	}

	String pathFor(const Options& opt, Emit e) {
		String base = baseName(opt.output);
		switch(e) {
		case Emit::Tok:
			return base + ".tok";
		case Emit::Ast:
			return base + ".ast";
		case Emit::C:
			return base + ".c";
		case Emit::X86:
			return opt.output;
		}
		__builtin_unreachable();
	}

	void usage(std::ostream& os) {
		os << "usage: ratcc [options] <input.c>\n"
					"  -o <file>             output base (default a.out); x86 object goes here\n"
					"  -emit <k,...>         any of: tok, ast, c, x86 (comma-separated)\n"
					"  -O0                   no optimization (default)\n"
					"  -O1                   all optimization passes + graph-coloring regalloc\n"
					"  -f<pass>              enable one opt pass: fold, gvn, sccp,\n"
					"                        simplifycfg, memoryopt, inline\n"
					"  -I<dir> -D<m> -U<m>   preprocessor options\n"
					"  -E                    preprocess only\n"
					"  -ftime-passes         print per-pass timing to stderr\n"
					"  -help                 show this help\n"
					"  -version              show version and build date\n";
	}

	I32 parseArgs(I32 argc, char** argv, Options& opt, B32& stop) {
		stop = true;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			auto value = [&](U32 prefix) -> String {
				return arg.size() > prefix ? arg.substr(prefix) : (++i < argc ? argv[i] : "");
			};
			auto next = [&]() -> String { return ++i < argc ? argv[i] : ""; };
			if(arg == "-help" || arg == "--help" || arg == "-h") {
				usage(std::cout);
				return 0;
			} else if(arg == "-version" || arg == "--version") {
				std::cout << kVersion << " (built " << __DATE__ << " " << __TIME__ << ")\n";
				return 0;
			} else if(arg.rfind("-o", 0) == 0) {
				opt.output = value(2);
			} else if(arg == "-emit") {
				String err;
				if(!parseEmit(next(), opt.emits, err)) {
					std::cerr << "ratcc: " << err << "\n";
					return 2;
				}
			} else if(arg == "-O0") {
				opt.optLevel = 0;
			} else if(arg == "-O1" || arg == "-O") {
				opt.optLevel = 1;
			} else if(arg == "-ftime-passes") {
				opt.timePasses = true;
			} else if(arg.rfind("-f", 0) == 0) {
				String pass = arg.substr(2);
				if(!isOptPass(pass)) {
					std::cerr << "ratcc: unknown optimization '-f" << pass << "'\n";
					return 2;
				}
				opt.extraPasses.push_back(pass);
			} else if(arg == "-E") {
				opt.preprocessOnly = true;
			} else if(arg.rfind("-I", 0) == 0) {
				opt.pp.includeDirs.push_back(value(2));
			} else if(arg.rfind("-D", 0) == 0) {
				opt.pp.defines.push_back(value(2));
			} else if(arg.rfind("-U", 0) == 0) {
				opt.pp.undefs.push_back(value(2));
			} else if(!arg.empty() && arg[0] == '-') {
				std::cerr << "ratcc: unknown option '" << arg << "'\n";
				return 2;
			} else if(opt.input.empty()) {
				opt.input = arg;
			} else {
				std::cerr << "ratcc: unexpected extra argument '" << arg << "'\n";
				return 2;
			}
		}
		if(opt.emits.empty() && !opt.preprocessOnly)
			opt.emits.push_back(Emit::X86); // compile by default
		stop = false;
		return 0;
	}

	B32 readInput(const Options& opt, String& source, String& path) {
		if(opt.input.empty()) {
			path = "<stdin>";
			if(!readAll(std::cin, source)) {
				std::cerr << "ratcc: failed to read stdin\n";
				return false;
			}
			return true;
		}
		path = opt.input;
		std::ifstream f(opt.input);
		if(!f) {
			std::cerr << "ratcc: cannot open '" << opt.input << "'\n";
			return false;
		}
		if(!readAll(f, source)) {
			std::cerr << "ratcc: failed to read '" << opt.input << "'\n";
			return false;
		}
		return true;
	}

	I32 emitTokens(const String& path, const String& source, std::ostream& os) {
		Lexer lex(source.data(), (U32)source.size(), path);
		for(;;) {
			Token tok = lex.next();
			os << tok.line << ":" << tok.col << "\t" << tokKindName(tok.kind);
			if(tok.kind == TokKind::Error) {
				os << "\t" << lex.error() << "\n";
				return 1;
			}
			if(tok.kind == TokKind::Eof) {
				os << "\n";
				return 0;
			}
			os << "\t'" << lex.text(tok) << "'\n";
		}
	}

	TransUnit*
	parse(const String& path, const String& source, Arena& arena, const TargetInfo& target) {
		Lexer lex(source.data(), (U32)source.size(), path);
		Parser parser(lex, arena, target);
		TransUnit* unit = parser.parseUnit();
		if(!unit)
			std::cerr << parser.error() << "\n";
		return unit;
	}

	I32 emitAstText(const String& path, const String& source, std::ostream& os) {
		Arena arena;
		Generic64 target;
		TransUnit* unit = parse(path, source, arena, target);
		if(!unit)
			return 1;
		dumpAst(*unit, os);
		return 0;
	}

	I32 emitViaModule(
			const Options& opt, const String& path, const String& source, Emit kind, std::ostream& os) {
		Generic64 generic;
		X86Target x86;
		const TargetInfo& target = (kind == Emit::X86) ? (const TargetInfo&)x86 : generic;

		Arena arena;
		TransUnit* unit = parse(path, source, arena, target);
		if(!unit)
			return 1;

		Module mod;
		mod.setTarget(&target);
		Emitter emitter(mod);
		if(!emitter.emit(*unit)) {
			std::cerr << path << ": " << emitter.error() << "\n";
			return 1;
		}

		CompileOptions copt;
		copt.backend = (kind == Emit::X86) ? Backend::X86 : Backend::C;
		copt.regAlloc = (opt.optLevel >= 1) ? RegAlloc::Graph : RegAlloc::Linear;
		copt.optPasses = buildOptPasses(opt);

		// Keep the PassManager local so we can print the timing report from it.
		PassManager pm(target);
		composePipeline(pm, copt, os);
		pm.run(mod);
		if(opt.timePasses)
			pm.printTimingReport(std::cerr);
		return 0;
	}

	I32 emitOne(const Options& opt, const String& path, const String& source, Emit kind) {
		String out = pathFor(opt, kind);
		B32 binary = (kind == Emit::X86);
		std::ofstream file(out, binary ? std::ios::binary : std::ios::out);
		if(!file) {
			std::cerr << "ratcc: cannot write '" << out << "'\n";
			return 1;
		}
		switch(kind) {
		case Emit::Tok:
			return emitTokens(path, source, file);
		case Emit::Ast:
			return emitAstText(path, source, file);
		case Emit::C:
		case Emit::X86:
			return emitViaModule(opt, path, source, kind, file);
		}
		__builtin_unreachable();
	}
} // namespace

I32 main(I32 argc, char** argv) {
	Options opt;
	B32 stop = false;
	if(I32 rc = parseArgs(argc, argv, opt, stop); stop)
		return rc;

	String source, path;
	if(!readInput(opt, source, path))
		return 1;

	String pped, ppErr;
	if(!preprocess(path, source, opt.pp, pped, ppErr)) {
		std::cerr << "ratcc: " << ppErr << "\n";
		return 1;
	}
	source = pped;

	if(opt.preprocessOnly) {
		std::cout << source;
		return 0;
	}

	for(Emit kind : opt.emits)
		if(I32 rc = emitOne(opt, path, source, kind))
			return rc;
	return 0;
}
