#include "Lexer.h"

using namespace rat;
using namespace rat::cc;

namespace {
	B32 readAll(std::istream& in, String& out) {
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return (B32)!in.bad();
	}

	I32 dumpTokens(const String& path, const String& source) {
		Lexer lex(source.data(), (U32)source.size(), path);
		for (;;) {
			Token tok = lex.next();
			std::cout << tok.line << ":" << tok.col << "\t" << tokKindName(tok.kind);
			if (tok.kind == TokKind::Error) {
				std::cout << "\t" << lex.error() << "\n";
				return 1;
			}
			if (tok.kind == TokKind::Eof) {
				std::cout << "\n";
				break;
			}
			std::cout << "\t'" << lex.text(tok) << "'\n";
		}
		return 0;
	}
} // namespace

I32 main(I32 argc, char** argv) {
	String mode = "-dump-tokens";
	String inputPath;

	for (I32 i = 1; i < argc; ++i) {
		String arg = argv[i];
		if (arg == "-dump-tokens") {
			mode = arg;
		} else if (!arg.empty() && arg[0] == '-') {
			std::cerr << "ratcc: unknown option '" << arg << "'\n";
			return 2;
		} else if (inputPath.empty()) {
			inputPath = arg;
		} else {
			std::cerr << "ratcc: unexpected extra argument '" << arg << "'\n";
			return 2;
		}
	}

	String source;
	if (inputPath.empty()) {
		if (!readAll(std::cin, source)) {
			std::cerr << "ratcc: failed to read stdin\n";
			return 1;
		}
		inputPath = "<stdin>";
	} else {
		std::ifstream f(inputPath);
		if (!f) {
			std::cerr << "ratcc: cannot open '" << inputPath << "'\n";
			return 1;
		}
		if (!readAll(f, source)) {
			std::cerr << "ratcc: failed to read '" << inputPath << "'\n";
			return 1;
		}
	}

	if (mode == "-dump-tokens")
		return dumpTokens(inputPath, source);

	std::cerr << "ratcc: nothing to do\n";
	return 2;
}
