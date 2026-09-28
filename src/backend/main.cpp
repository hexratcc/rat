#include "rat.h"

#include "cli.h"
#include "ir/text_parser.h"

#include <iostream>

using namespace rat;

namespace detail {
	static const C8* kTool = "rat";

	struct Options {
		String passSpec;
		String emitKind = "text";
		String inputPath;
		String outputPath;
		B32 stats = false;
		B32 doVerify = false;
	};

	void usage(std::ostream& os) {
		os << "usage: rat [options] [input.rat]\n"
					"\n"
					"  -passes <a,b,...>   pass pipeline to run, in order\n"
					"  -emit <text|dot>    output format (default text)\n"
					"  -o <file>           output file (default stdout)\n"
					"  -stats              report per-pass changes to stderr\n"
					"  -verify             append a verify pass\n"
					"  -list-passes        list available passes and exit\n"
					"  -h, -help           show this help\n"
					"  -version            show version\n";
	}

	B32 valueArg(I32 argc, C8** argv, I32& i, Options& o) {
		return cli::value(kTool, argc, argv, i, "-passes", o.passSpec) ||
					 cli::value(kTool, argc, argv, i, "-emit", o.emitKind) ||
					 cli::value(kTool, argc, argv, i, "-o", o.outputPath);
	}

	I32 execute(const Options& o) {
		if(o.emitKind != "text" && o.emitKind != "dot")
			return cli::error(kTool, "unknown -emit value '" + o.emitKind + "' (expected text or dot)");
		String emitter = o.emitKind == "dot" ? "graph-emitter" : o.emitKind + "-emitter";

		String source;
		if(!cli::readInput(kTool, o.inputPath, source))
			return 1;

		Generic64 target;
		Module module;
		if(!parseText(source, module, std::cerr))
			return std::cerr << kTool << ": parse error\n", 1;

		std::ofstream outFile;
		if(!o.outputPath.empty() && !cli::openOutput(kTool, o.outputPath, outFile))
			return 1;
		std::ostream& out = o.outputPath.empty() ? std::cout : outFile;

		PassManager pm(target);
		String err;
		if(!buildPipeline(pm, o.passSpec, out, err))
			return cli::error(kTool, err);
		if(o.doVerify)
			pm.add<VerifyPass>(std::cerr);
		pm.add(createPass(emitter, out));

		pm.run(module, o.stats ? &std::cerr : nullptr);
		return 0;
	}

	I32 run(I32 argc, C8** argv) {
		Options o;
		for(I32 i = 1; i < argc; ++i) {
			String arg = argv[i];
			cli::stdFlags(kTool, arg, usage);
			if(arg == "-list-passes")
				return listPasses(std::cout, false), 0;
			if(arg == "-stats")
				o.stats = true;
			else if(arg == "-verify")
				o.doVerify = true;
			else if(valueArg(argc, argv, i, o))
				;
			else if(arg.size() > 1 && arg[0] == '-')
				return cli::error(kTool, "unknown option '" + arg + "'");
			else if(o.inputPath.empty())
				o.inputPath = arg;
			else
				return cli::error(kTool, "unexpected extra argument '" + arg + "'");
		}
		return execute(o);
	}
} // namespace detail

I32 main(I32 argc, C8** argv) {
	return cli::guardedMain(::detail::kTool, ::detail::run, argc, argv);
}
