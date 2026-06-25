#include "rat.h"

#include "IR/TextParser.h"
#include "Support/StringUtil.h"

using namespace rat;

namespace {
	void usage(std::ostream& os, const char* prog) {
		os << "usage: " << prog << " [options] [input.rat]\n"
			 << "  -passes=<a,b,...>   pipeline to run, in order\n"
			 << "  -emit=<text|c|dot>  output format (default: text)\n"
			 << "  -o <file>           output file (default: stdout)\n"
			 << "  -stats              report per-pass changes to stderr\n"
			 << "  -verify             append a verify pass\n"
			 << "  -list-passes        list available passes and exit\n"
			 << "  -h, --help          show this help\n";
	}

	void listPasses(std::ostream& os) {
		os << "passes:\n";
		for (const PassRegistry::Entry& e : passRegistry().entries())
			os << "  " << e.name << std::string(14 > e.name.size() ? 14 - e.name.size() : 1, ' ')
				 << e.description << "\n";
	}

	void addEmitter(PassManager& pm, const String& kind, std::ostream& os) {
		if (kind == "c")
			pm.add<CEmitterPass>(os);
		else if (kind == "dot")
			pm.add<GraphEmitterPass>(os);
		else
			pm.add<TextEmitterPass>(os);
	}
} // namespace

I32 main(I32 argc, char** argv) {
	String passSpec;
	String emitKind = "text";
	String inputPath;
	String outputPath;
	B32 stats = false;
	B32 doVerify = false;

	for (I32 i = 1; i < argc; ++i) {
		String arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			usage(std::cout, argv[0]);
			return 0;
		} else if (arg == "-list-passes") {
			listPasses(std::cout);
			return 0;
		} else if (arg == "-stats") {
			stats = true;
		} else if (arg == "-verify") {
			doVerify = true;
		} else if (arg.rfind("-passes=", 0) == 0) {
			passSpec = arg.substr(8);
		} else if (arg.rfind("-emit=", 0) == 0) {
			emitKind = arg.substr(6);
		} else if (arg == "-o") {
			if (++i >= argc) {
				std::cerr << "rat: -o requires a file argument\n";
				return 2;
			}
			outputPath = argv[i];
		} else if (!arg.empty() && arg[0] == '-') {
			std::cerr << "rat: unknown option '" << arg << "'\n";
			usage(std::cerr, argv[0]);
			return 2;
		} else if (inputPath.empty()) {
			inputPath = arg;
		} else {
			std::cerr << "rat: unexpected extra argument '" << arg << "'\n";
			return 2;
		}
	}

	if (emitKind != "text" && emitKind != "c" && emitKind != "dot") {
		std::cerr << "rat: unknown -emit value '" << emitKind << "' (expected text, c, or dot)\n";
		return 2;
	}

	String source;
	if (inputPath.empty()) {
		if (!readAll(std::cin, source)) {
			std::cerr << "rat: failed to read stdin\n";
			return 1;
		}
	} else {
		std::ifstream f(inputPath);
		if (!f) {
			std::cerr << "rat: cannot open '" << inputPath << "'\n";
			return 1;
		}
		if (!readAll(f, source)) {
			std::cerr << "rat: failed to read '" << inputPath << "'\n";
			return 1;
		}
	}

	Generic64 target;
	Module module;
	module.setTarget(&target);
	if (!parseText(source, module, std::cerr)) {
		std::cerr << "rat: parse error\n";
		return 1;
	}

	std::ofstream outFile;
	if (!outputPath.empty()) {
		outFile.open(outputPath);
		if (!outFile) {
			std::cerr << "rat: cannot open '" << outputPath << "' for writing\n";
			return 1;
		}
	}
	std::ostream& out = outputPath.empty() ? std::cout : outFile;

	PassManager pm;
	String err;
	if (!buildPipeline(pm, passSpec, out, err)) {
		std::cerr << "rat: " << err << "\n";
		return 2;
	}
	if (doVerify)
		pm.add<VerifyPass>(std::cerr);
	addEmitter(pm, emitKind, out);

	pm.run(module, stats ? &std::cerr : nullptr);
	return 0;
}
