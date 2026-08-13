#ifndef RAT_BASE_CLI_H
#define RAT_BASE_CLI_H

#include <fstream>

#include "core.h"
#include "git_hash.h"
#include "string.h"

namespace rat::cli {
	inline void printVersion(std::ostream& os, const C8* tool) {
		os << tool << " 0.1 (" << GIT_HASH << ", built " << __DATE__ << " " << __TIME__ << ")\n";
	}

	// "<tool>: <msg>", returns 2
	inline I32 error(const C8* tool, const String& msg) {
		return std::cerr << tool << ": " << msg << "\n", 2;
	}

	// error + exit(2)
	[[noreturn]] inline void die(const C8* tool, const String& msg) { std::exit(error(tool, msg)); }

	// -h/-help/--help and -version/--version, shared by every tool
	inline void stdFlags(const C8* tool, const String& arg, void (*usage)(std::ostream&)) {
		if(arg == "-h" || arg == "-help" || arg == "--help")
			usage(std::cout), std::exit(0);
		if(arg == "-version" || arg == "--version")
			printVersion(std::cout, tool), std::exit(0);
	}

	// -name <v> and -name=<v>, advances i past a consumed argument
	inline B32 value(const C8* tool, I32 argc, C8** argv, I32& i, const C8* name, String& out) {
		String arg = argv[i];
		U64 n = std::strlen(name);
		if(arg.rfind(name, 0) == 0 && arg.size() > n && arg[n] == '=')
			return out = arg.substr(n + 1), true;
		if(arg != name)
			return false;
		if(++i >= argc)
			die(tool, arg + " expects an argument");
		return out = argv[i], true;
	}

	// "" reads stdin
	inline B32 readInput(const C8* tool, const String& path, String& source) {
		if(path.empty()) {
			if(readAll(std::cin, source))
				return true;
			return error(tool, "cannot read '<stdin>'"), false;
		}
		std::ifstream f(path);
		if(f && readAll(f, source))
			return true;
		return error(tool, "cannot read '" + path + "'"), false;
	}

	inline B32 openOutput(const C8* tool, const String& path, std::ofstream& f, B32 binary = false) {
		f.open(path, binary ? std::ios::binary : std::ios::out);
		return f || (std::cerr << tool << ": cannot write '" << path << "'\n", false);
	}

	inline I32 guardedMain(const C8* tool, I32 (*run)(I32, C8**), I32 argc, C8** argv) {
		try {
			return run(argc, argv);
		} catch(const std::exception& e) {
			return std::cerr << tool << ": internal error: " << e.what() << "\n", 3;
		} catch(...) {
			return std::cerr << tool << ": internal error\n", 3;
		}
	}
} // namespace rat::cli

#endif
