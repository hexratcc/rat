#include "Ast.h"
#include "Emit.h"
#include "Lexer.h"
#include "Parser.h"

#include "rat.h"

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

	I32 dumpAst(const String& path, const String& source) {
		Lexer lex(source.data(), (U32)source.size(), path);
		Arena arena;
		Parser parser(lex, arena);
		TransUnit* unit = parser.parseUnit();
		if (!unit) {
			std::cerr << parser.error() << "\n";
			return 1;
		}
		dumpAst(*unit, std::cout);
		return 0;
	}

	I32 emitIr(const String& path, const String& source, const String& passSpec) {
		Lexer lex(source.data(), (U32)source.size(), path);
		Arena arena;
		Parser parser(lex, arena);
		TransUnit* unit = parser.parseUnit();
		if (!unit) {
			std::cerr << parser.error() << "\n";
			return 1;
		}

		Generic64 target;
		Module mod;
		mod.setTarget(&target);
		Emitter emitter(mod);
		if (!emitter.emit(*unit)) {
			std::cerr << path << ": " << emitter.error() << "\n";
			return 1;
		}

		if (!passSpec.empty()) {
			PassManager pm;
			std::ostringstream sink;
			String err;
			if (!buildPipeline(pm, passSpec, sink, err)) {
				std::cerr << "ratcc: " << err << "\n";
				return 2;
			}
			pm.run(mod);
		}

		emitText(mod, std::cout);
		return 0;
	}
} // namespace

I32 main(I32 argc, char** argv) {
	String mode = "-dump-tokens";
	String inputPath;
	String passSpec;

	for (I32 i = 1; i < argc; ++i) {
		String arg = argv[i];
		if (arg == "-dump-tokens" || arg == "-dump-ast" || arg == "-emit-ir") {
			mode = arg;
		} else if (arg.rfind("-passes=", 0) == 0) {
			passSpec = arg.substr(8);
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
	if (mode == "-dump-ast")
		return dumpAst(inputPath, source);
	if (mode == "-emit-ir")
		return emitIr(inputPath, source, passSpec);

	std::cerr << "ratcc: nothing to do\n";
	return 2;
}
