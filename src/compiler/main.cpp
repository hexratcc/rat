#include "builtin_headers.h"
#include "compile.h"
#include "host.h"
#include "predef.h"

#include "emit/emit.h"
#include "lex/lexer.h"
#include "lex/preprocess.h"
#include "parse/parser.h"
#include <chrono>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "cli.h"
#include "rat.h"
#include "string.h"

using namespace rat;
using namespace rat::cc;

namespace detail {
	using PhaseClock = std::chrono::steady_clock;

	enum struct Emit { Tok, Ast, X86 };

	struct Options {
		String input;						 // root input file ("" => stdin)
		String output = "a.out"; // -o base; x86 object lands here
		List<Emit> emits;				 // requested -emit kinds (in order)
		U32 optLevel = 0;
		String passSpec;					// -fpasses=: exact opt pipeline
		String machineSpec;				// -fmachine-passes=: exact x86 machine pipeline
		List<String> extraPasses; // individual -f<pass> requests (in order)
		B32 timePasses = false, preprocessOnly = false;
		B32 noStdInc = false, noPredefs = false; // -nostdinc / -undef: skip builtin headers / predefs
		String targetSpec;
		PpOptions pp;
	};

	static const C8* kTool = "ratcc";

	F64 msSince(PhaseClock::time_point t0) {
		return std::chrono::duration<F64, std::milli>(PhaseClock::now() - t0).count();
	}
	// frontend phase times, printed with -ftime-passes
	static F64 gReadMs = 0, gPpMs = 0;

	List<UniquePtr<Pass>> buildOptPasses(const Options& opt) {
		B32 useDefault = opt.passSpec.empty() && opt.optLevel >= 1;
		List<String> names = useDefault ? defaultOptPipeline() : splitTokens(opt.passSpec);
		names.insert(names.end(), opt.extraPasses.begin(), opt.extraPasses.end());
		List<UniquePtr<Pass>> passes;
		for(const String& name : names) {
			passes.push_back(createPass(name, std::cerr));
			if(!passes.back())
				cli::die(kTool, "unknown pass '" + name + "' (see -list-passes)");
		}
		return passes;
	}

	const String kEmitNames[] = {"tok", "ast", "x86"}; // Emit order

	void parseEmit(const String& spec, List<Emit>& out) {
		for(const String& k : splitTokens(spec)) {
			U32 e = (U32)(std::find(kEmitNames, kEmitNames + 3, k) - kEmitNames);
			if(e == 3)
				cli::die(kTool, "unknown -emit kind '" + k + "'");
			out.push_back((Emit)e);
		}
	}

	String pathFor(const Options& opt, Emit e) {
		if(e == Emit::X86)
			return opt.output;
		U64 dot = opt.output.rfind('.'), slash = opt.output.rfind('/');
		String base = opt.output;
		if(dot != String::npos && (slash == String::npos || dot > slash))
			base = base.substr(0, dot);
		return base + "." + kEmitNames[(U32)e];
	}

	void usage(std::ostream& os) {
		os << "usage: ratcc [options] <input.c>\n"
					"\n"
					"  -o <file>             output path (default a.out)\n"
					"  -E                    preprocess only\n"
					"  -I<dir> -D<m> -U<m>   preprocessor include dir, define, undefine\n"
					"  -O0 / -O1             optimization level (default -O0)\n"
					"  -nostdinc             do not provide the builtin C standard headers\n"
					"  -undef                do not predefine the builtin target macros\n"
					"  -target <triple>      x86_64-linux (default) or x86_64-windows\n"
					"  -h, -help             show this help\n"
					"  -version              show version\n"
					"\n"
					"rat extensions:\n"
					"  -emit <k,...>         any of: tok, ast, x86 (default x86);\n"
					"                        side outputs derive from the -o base name\n"
					"  -f<pass>              append one opt pass (see -list-passes)\n"
					"  -fpasses=<a,b,...>    exact opt pipeline, overrides the -O selection\n"
					"  -fmachine-passes=<a,b,...>\n"
					"                        exact machine pipeline for the x86 backend\n"
					"  -ftime-passes         print per-phase and per-pass timing to stderr\n"
					"  -list-passes          list available passes and exit\n";
	}

	Options parseArgs(I32 argc, C8** argv) {
		Options opt;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			// -X<v> and -X <v>
			auto rest = [&](U32 prefix) -> String {
				if(arg.size() > prefix)
					return arg.substr(prefix);
				if(++i >= argc)
					cli::die(kTool, arg + " expects an argument");
				return argv[i];
			};
			auto value = [&](const C8* name, String& out) -> B32 {
				return cli::value(kTool, argc, argv, i, name, out);
			};
			auto flag = [&](const C8* name, B32& out) -> B32 { return arg == name && (out = true); };
			cli::stdFlags(kTool, arg, usage);
			if(arg == "-list-passes")
				listPasses(std::cout, true), std::exit(0);
			else if(arg == "-O0" || arg == "-O1" || arg == "-O")
				opt.optLevel = arg != "-O0";
			else if(flag("-E", opt.preprocessOnly) || flag("-nostdinc", opt.noStdInc) ||
							flag("-undef", opt.noPredefs) || flag("-ftime-passes", opt.timePasses))
				;
			else if(String spec; value("-emit", spec))
				parseEmit(spec, opt.emits);
			else if(value("-target", opt.targetSpec) || value("-fpasses", opt.passSpec) ||
							value("-fmachine-passes", opt.machineSpec))
				;
			else if(arg.rfind("-f", 0) == 0 && createPass(arg.substr(2), std::cerr))
				opt.extraPasses.push_back(arg.substr(2));
			else if(arg.rfind("-o", 0) == 0)
				opt.output = rest(2);
			else if(arg.rfind("-I", 0) == 0)
				opt.pp.includeDirs.push_back(rest(2));
			else if(arg.rfind("-D", 0) == 0)
				opt.pp.defines.push_back(rest(2));
			else if(arg.rfind("-U", 0) == 0)
				opt.pp.undefs.push_back(rest(2));
			else if(arg.size() > 1 && arg[0] == '-')
				cli::die(kTool, "unknown option '" + arg + "'");
			else if(opt.input.empty())
				opt.input = arg;
			else
				cli::die(kTool, "unexpected extra argument '" + arg + "'");
		}
		if(opt.emits.empty() && !opt.preprocessOnly)
			opt.emits.push_back(Emit::X86); // compile by default
		return opt;
	}

	I32 emitTokens(const String& path, const String& source, std::ostream& os) {
		Lexer lex(source.data(), (U32)source.size(), path);
		for(;;) {
			Token tok = lex.next();
			os << tok.line << ":" << tok.col << "\t" << tokKindName(tok.kind);
			if(tok.kind == TokKind::Error)
				return os << "\t" << lex.error() << "\n", 1;
			if(tok.kind == TokKind::Eof)
				return os << "\n", 0;
			os << "\t'" << lex.text(tok) << "'\n";
		}
	}

	TransUnit* parse(TokenStream& ts, Arena& arena) {
		ts.reset();
		Parser parser(ts, arena, TargetLayout::forTriple(hostTargetTriple()));
		TransUnit* unit = parser.parseUnit();
		if(!unit)
			std::cerr << parser.error() << "\n";
		return unit;
	}

	I32 emitAstText(TokenStream& ts, std::ostream& os) {
		Arena arena;
		TransUnit* unit = parse(ts, arena);
		if(!unit)
			return 1;
		dumpAst(*unit, os);
		return 0;
	}

	I32 emitViaModule(const Options& opt, TokenStream& ts, std::ostream& os) {
		X86Target target(hostTargetTriple());

		Arena arena;
		PhaseClock::time_point t0 = PhaseClock::now();
		TransUnit* unit = parse(ts, arena);
		F64 parseMs = msSince(t0);
		if(!unit)
			return 1;

		Module mod;
		Emitter emitter(mod, TargetLayout::forTriple(hostTargetTriple()));
		t0 = PhaseClock::now();
		B32 emitOk = emitter.emit(*unit);
		F64 emitMs = msSince(t0);
		for(const String& w : emitter.warnings())
			std::cerr << ts.file() << ": " << w << "\n";
		if(!emitOk)
			return std::cerr << ts.file() << ": " << emitter.error() << "\n", 1;
		if(opt.timePasses)
			std::cerr << "frontend: read " << gReadMs << "ms, preprocess+tokens " << gPpMs << "ms, parse "
								<< parseMs << "ms, ast-to-ir " << emitMs << "ms\n";

		CompileOptions copt;
		copt.optPasses = buildOptPasses(opt);
		for(const String& name : splitTokens(opt.machineSpec)) {
			copt.machinePasses.push_back(createMachinePass(name, os));
			if(!copt.machinePasses.back())
				cli::die(kTool, "unknown machine pass '" + name + "' (see -list-passes)");
		}

		// keep the pass manager local so we can print the timing report from it
		PassManager pm(target);
		composePipeline(pm, copt, os);
		pm.run(mod);
		if(opt.timePasses)
			pm.printTimingReport(std::cerr);
		return 0;
	}

	I32 emitOne(
			const Options& opt, const String& path, const String& pped, TokenStream* ts, Emit kind) {
		std::ofstream file;
		if(!cli::openOutput(kTool, pathFor(opt, kind), file, kind == Emit::X86))
			return 1;
		if(kind == Emit::Tok)
			return emitTokens(path, pped, file);
		if(kind == Emit::Ast)
			return emitAstText(*ts, file);
		return emitViaModule(opt, *ts, file);
	}
} // namespace detail

static I32 run(I32 argc, C8** argv) {
	::detail::Options opt = ::detail::parseArgs(argc, argv);

	if(!opt.targetSpec.empty()) {
		TargetTriple triple;
		String err;
		if(!TargetTriple::parse(opt.targetSpec, triple, err))
			return cli::error(::detail::kTool, err);
		setHostTargetTriple(triple);
	}

	String source;
	::detail::PhaseClock::time_point tRead = ::detail::PhaseClock::now();
	if(!cli::readInput(::detail::kTool, opt.input, source))
		return 1;
	String path = opt.input.empty() ? "<stdin>" : opt.input;
	::detail::gReadMs = ::detail::msSince(tRead);

	if(!opt.noStdInc)
		opt.pp.includeDirs.push_back(builtinIncludeDir());
	if(!opt.noPredefs)
		source = builtinPredefs(hostTargetTriple()) + "#line 1 \"" + path + "\"\n" + source;

	// -E and -emit tok need serialized text; else parse the pp token stream directly
	B32 needText = opt.preprocessOnly, needToks = false;
	for(::detail::Emit kind : opt.emits)
		if(kind == ::detail::Emit::Tok)
			needText = true;
		else
			needToks = true;

	String pped, ppErr;
	TokenStream ts;
	::detail::PhaseClock::time_point tPp = ::detail::PhaseClock::now();
	B32 ppOk = (!needText || preprocess(path, source, opt.pp, pped, ppErr)) &&
						 (!needToks || preprocessToTokens(path, source, opt.pp, ts, ppErr));
	::detail::gPpMs = ::detail::msSince(tPp);
	if(!ppOk)
		return std::cerr << ::detail::kTool << ": " << ppErr << "\n", 1;
	if(opt.preprocessOnly)
		return std::cout << pped, 0;

	for(::detail::Emit kind : opt.emits)
		if(I32 rc = ::detail::emitOne(opt, path, pped, needToks ? &ts : nullptr, kind))
			return rc;
	return 0;
}

I32 main(I32 argc, C8** argv) {
#ifdef __GLIBC__
	// batch process: never return memory mid-run, keep big blocks off mmap
	mallopt(M_TRIM_THRESHOLD, 1 << 29);
	mallopt(M_MMAP_THRESHOLD, 1 << 29);
	mallopt(M_TOP_PAD, 1 << 26);
#endif
	return cli::guardedMain(::detail::kTool, run, argc, argv);
}
