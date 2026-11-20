#include "builtin_headers.h"
#include "predef.h"

#include "emit/emit.h"
#include "lex/token_stream.h"
#include "parse/parser.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
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

	struct Options {
		String input;						 // root input file ("" => stdin)
		String output = "a.out"; // -o, x86 object lands here
		U32 optLevel = 0;
		String passSpec;					// -fpasses=: exact opt pipeline
		String machineSpec;				// -fmachine-passes=: exact x86 machine pipeline
		List<String> extraPasses; // individual -f<pass> requests (in order)
		B32 timePasses = false;
		B32 passStats = false;
		B32 preprocessOnly = false;
		B32 noStdInc = false;	 // -nostdinc: skip builtin headers
		B32 noPredefs = false; // -undef: skip predefs
		U32 debugLevel = 0;
		String targetSpec; // -target
		PpOptions pp;
	};

	static const C8* kTool = "ratcc";

	F64 msSince(PhaseClock::time_point t0) {
		return std::chrono::duration<F64, std::milli>(PhaseClock::now() - t0).count();
	}
	// frontend phase times, printed with -ftime-passes
	static F64 gReadMs = 0, gPpMs = 0;

	TargetTriple defaultTriple() {
		TargetTriple t;
#if defined(_WIN32)
		t.os = OS::Windows; // native builds default to the host platform
#endif
		return t;
	}

	// the default target, -target overrides it
	TargetTriple& targetTriple() {
		static TargetTriple triple = defaultTriple();
		return triple;
	}

	// opt passes, then the x86 machine pipeline, -fmachine-passes replaces the default one
	void addPasses(PassManager& pm, const Options& opt, std::ostream& out) {
		B32 optimize = opt.optLevel >= 1;
		B32 useDefault = opt.passSpec.empty() && optimize;
		List<String> names = useDefault ? defaultOptPipeline() : splitTokens(opt.passSpec);
		names.insert(names.end(), opt.extraPasses.begin(), opt.extraPasses.end());
		for(const String& name : names)
			if(!pm.add(createPass(name, std::cerr)))
				cli::die(kTool, "unknown pass '" + name + "' (see -list-passes)");
		List<String> machineNames = splitTokens(opt.machineSpec);
		for(const String& name : machineNames)
			if(!pm.add(createMachinePass(name, out)))
				cli::die(kTool, "unknown machine pass '" + name + "' (see -list-passes)");
		if(!machineNames.empty())
			return;
		pm.add(createMachinePass("x86-lower", out));
		MachinePass* regalloc = pm.add(createMachinePass("regalloc", out));
		static_cast<RegAllocPass*>(regalloc)->setLocal(!optimize);
		if(optimize) {
			pm.add(createMachinePass("x86-peephole", out));
			pm.add(createMachinePass("x86-layout", out));
		}
		pm.add(createMachinePass("x86-encode", out));
	}

	void usage(std::ostream& os) {
		os << "usage: ratcc [options] <input.c>\n"
					"\n"
					"  -o <file>             output path (default a.out)\n"
					"  -E                    preprocess only\n"
					"  -I<dir> -D<m> -U<m>   preprocessor include dir, define, undefine\n"
					"  -O0 / -O1             optimization level (default -O0)\n"
					"  -g[0..3]              debug info (DWARF), -g at -O0 keeps locals in memory,\n"
					"                        also -ggdb[0..3], -gline-tables-only, -gdwarf[-5]\n"
					"  -nostdinc             do not provide the builtin C standard headers\n"
					"  -undef                do not predefine the builtin target macros\n"
					"  -target <triple>      x86_64-linux (default) or x86_64-windows\n"
					"  -h, -help             show this help\n"
					"  -version              show version\n"
					"\n"
					"rat extensions:\n"
					"  -f<pass>              append one opt pass (see -list-passes)\n"
					"  -fpasses=<a,b,...>    exact opt pipeline, overrides the -O selection\n"
					"  -fmachine-passes=<a,b,...>\n"
					"                        exact machine pipeline for the x86 backend\n"
					"  -ftime-passes         print per-phase and per-pass timing to stderr\n"
					"  -fpass-stats          print pass statistics to stderr\n"
					"  -list-passes          list available passes and exit\n";
	}

	// -X<v> and -X <v>
	String joinedValue(I32 argc, C8** argv, I32& i, U32 prefix) {
		String arg = argv[i];
		if(arg.size() > prefix)
			return arg.substr(prefix);
		if(++i >= argc)
			cli::die(kTool, arg + " expects an argument");
		return argv[i];
	}

	// the gcc and clang spellings, the last one wins
	U32 parseDebugLevel(const String& arg) {
		if(arg == "-g" || arg == "-ggdb" || arg == "-gdwarf" || arg == "-gdwarf-5")
			return 2;
		if(arg == "-gline-tables-only")
			return 1;
		U32 digit = arg.rfind("-ggdb", 0) == 0 ? 5 : 2;
		if(arg.size() != digit + 1 || arg[digit] < '0' || arg[digit] > '3')
			cli::die(kTool, "unknown option '" + arg + "'");
		return (U32)(arg[digit] - '0');
	}

	Options parseArgs(I32 argc, C8** argv) {
		Options opt;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			cli::stdFlags(kTool, arg, usage);
			if(arg == "-list-passes") {
				listPasses(std::cout, true);
				std::exit(0);
			} else if(arg == "-O0" || arg == "-O1" || arg == "-O")
				opt.optLevel = arg != "-O0";
			else if(arg == "-E")
				opt.preprocessOnly = true;
			else if(arg == "-nostdinc")
				opt.noStdInc = true;
			else if(arg == "-undef")
				opt.noPredefs = true;
			else if(arg == "-ftime-passes")
				opt.timePasses = true;
			else if(arg == "-fpass-stats")
				opt.passStats = true;
			else if(arg.rfind("-g", 0) == 0)
				opt.debugLevel = parseDebugLevel(arg);
			else if(cli::value(kTool, argc, argv, i, "-target", opt.targetSpec) ||
							cli::value(kTool, argc, argv, i, "-fpasses", opt.passSpec) ||
							cli::value(kTool, argc, argv, i, "-fmachine-passes", opt.machineSpec))
				;
			else if(arg.rfind("-f", 0) == 0 && createPass(arg.substr(2), std::cerr))
				opt.extraPasses.push_back(arg.substr(2));
			else if(arg.rfind("-o", 0) == 0)
				opt.output = joinedValue(argc, argv, i, 2);
			else if(arg.rfind("-I", 0) == 0)
				opt.pp.includeDirs.push_back(joinedValue(argc, argv, i, 2));
			else if(arg.rfind("-D", 0) == 0)
				opt.pp.defines.push_back(joinedValue(argc, argv, i, 2));
			else if(arg.rfind("-U", 0) == 0)
				opt.pp.undefs.push_back(joinedValue(argc, argv, i, 2));
			else if(arg.size() > 1 && arg[0] == '-')
				cli::die(kTool, "unknown option '" + arg + "'");
			else if(opt.input.empty())
				opt.input = arg;
			else
				cli::die(kTool, "unexpected extra argument '" + arg + "'");
		}
		return opt;
	}

	TransUnit* parse(TokenStream& ts, Arena& arena) {
		Parser parser(ts, arena, TargetLayout::forTriple(targetTriple()));
		TransUnit* unit = parser.parseUnit();
		if(!unit)
			std::cerr << parser.error() << "\n";
		return unit;
	}

	I32 emitViaModule(const Options& opt, TokenStream& ts, std::ostream& os) {
		X86Target target(targetTriple());

		Arena& arena = *new Arena;
		PhaseClock::time_point t0 = PhaseClock::now();
		TransUnit* unit = parse(ts, arena);
		F64 parseMs = msSince(t0);
		if(!unit)
			return 1;

		Module& mod = *new Module;
		Emitter emitter(mod, TargetLayout::forTriple(targetTriple()));
		if(opt.debugLevel)
			emitter.enableDebug(ts, opt.debugLevel > 1 && opt.optLevel == 0, opt.debugLevel > 1);
		t0 = PhaseClock::now();
		B32 emitOk = emitter.emit(*unit);
		F64 emitMs = msSince(t0);
		for(const String& w : emitter.warnings())
			std::cerr << ts.file() << ": " << w << "\n";
		if(!emitOk) {
			std::cerr << ts.file() << ": " << emitter.error() << "\n";
			return 1;
		}
		if(opt.timePasses) {
			std::cerr << "frontend: read " << gReadMs << "ms, preprocess+tokens " << gPpMs << "ms, ";
			std::cerr << "parse " << parseMs << "ms, ast-to-ir " << emitMs << "ms\n";
		}

		PassManager& pm = *new PassManager(target);
		addPasses(pm, opt, os);
		pm.run(mod);
		if(opt.passStats)
			pm.printStats(mod, std::cerr);
		if(opt.timePasses)
			pm.printTimingReport(std::cerr);
		return 0;
	}

	I32 run(I32 argc, C8** argv) {
		Options opt = parseArgs(argc, argv);

		if(!opt.targetSpec.empty()) {
			TargetTriple triple;
			String err;
			if(!TargetTriple::parse(opt.targetSpec, triple, err))
				return cli::error(kTool, err);
			targetTriple() = triple;
		}

		String source;
		PhaseClock::time_point tRead = PhaseClock::now();
		if(!cli::readInput(kTool, opt.input, source))
			return 1;
		String path = opt.input.empty() ? "<stdin>" : opt.input;
		gReadMs = msSince(tRead);

		if(!opt.noStdInc)
			opt.pp.includeDirs.push_back(builtinIncludeDir());
		if(!opt.noPredefs)
			source = builtinPredefs(targetTriple()) + "#line 1 \"" + path + "\"\n" + source;

		String pped;
		String ppErr;
		TokenStream& ts = *new TokenStream; // never freed, as the compile state
		PhaseClock::time_point tPp = PhaseClock::now();
		String* text = opt.preprocessOnly ? &pped : nullptr; // only -E needs serialized text
		B32 ppOk = preprocessToTokens(path, source, opt.pp, ts, text, ppErr);
		gPpMs = msSince(tPp);
		if(!ppOk) {
			cli::error(kTool, ppErr);
			return 1;
		}
		if(opt.preprocessOnly) {
			std::cout << pped;
			return 0;
		}

		std::ofstream file;
		if(!cli::openOutput(kTool, opt.output, file, true))
			return 1;
		return emitViaModule(opt, ts, file);
	}
} // namespace detail

I32 main(I32 argc, C8** argv) {
#ifdef __GLIBC__
	// batch process: never return memory mid-run, keep big blocks off mmap
	mallopt(M_TRIM_THRESHOLD, 1 << 29);
	mallopt(M_MMAP_THRESHOLD, 1 << 29);
	mallopt(M_TOP_PAD, 1 << 26);
#endif
	return cli::guardedMain(::detail::kTool, ::detail::run, argc, argv);
}
