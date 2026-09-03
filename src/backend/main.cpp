#include "rat.h"

#include "cli.h"
#include "ir/text_parser.h"

using namespace rat;

namespace detail {
	static const C8* kTool = "rat";

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

} // namespace detail

static I32 run(I32 argc, C8** argv) {
	String passSpec, emitKind = "text", inputPath, outputPath;
	B32 stats = false, doVerify = false;

	for(I32 i = 1; i < argc; ++i) {
		String arg = argv[i];
		auto value = [&](const C8* name, String& out) -> B32 {
			return cli::value(::detail::kTool, argc, argv, i, name, out);
		};
		cli::stdFlags(::detail::kTool, arg, ::detail::usage);
		if(arg == "-list-passes")
			return listPasses(std::cout, false), 0;
		if(arg == "-stats")
			stats = true;
		else if(arg == "-verify")
			doVerify = true;
		else if(value("-passes", passSpec) || value("-emit", emitKind) || value("-o", outputPath))
			;
		else if(arg.size() > 1 && arg[0] == '-')
			return cli::error(::detail::kTool, "unknown option '" + arg + "'");
		else if(inputPath.empty())
			inputPath = arg;
		else
			return cli::error(::detail::kTool, "unexpected extra argument '" + arg + "'");
	}

	if(emitKind != "text" && emitKind != "dot")
		return cli::error(
				::detail::kTool, "unknown -emit value '" + emitKind + "' (expected text or dot)");
	String emitter = emitKind == "dot" ? "graph-emitter" : emitKind + "-emitter";

	String source;
	if(!cli::readInput(::detail::kTool, inputPath, source))
		return 1;

	Generic64 target;
	Module module;
	if(!parseText(source, module, std::cerr))
		return std::cerr << ::detail::kTool << ": parse error\n", 1;

	std::ofstream outFile;
	if(!outputPath.empty() && !cli::openOutput(::detail::kTool, outputPath, outFile))
		return 1;
	std::ostream& out = outputPath.empty() ? std::cout : outFile;

	PassManager pm(target);
	String err;
	if(!buildPipeline(pm, passSpec, out, err))
		return cli::error(::detail::kTool, err);
	if(doVerify)
		pm.add<VerifyPass>(std::cerr);
	pm.add(createPass(emitter, out));

	pm.run(module, stats ? &std::cerr : nullptr);
	return 0;
}

I32 main(I32 argc, C8** argv) { return cli::guardedMain(::detail::kTool, run, argc, argv); }
