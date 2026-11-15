#ifndef RAT_BASE_CLI_H
#define RAT_BASE_CLI_H

#include <cstdlib>
#include <fstream>
#include <iostream>

#include "core.h"
#include "git_hash.h"
#include "string.h"

namespace rat::cli {
	inline void printVersion(std::ostream& os, const C8* tool) {
		os << tool << " 0.1 (" << GIT_HASH << ", built " << __DATE__ << " " << __TIME__ << ")\n";
	}

	// "<tool>: <msg>", returns 2
	inline I32 error(const C8* tool, const String& msg) {
		std::cerr << tool << ": " << msg << "\n";
		return 2;
	}

	// error + exit(2)
	[[noreturn]] inline void die(const C8* tool, const String& msg) { std::exit(error(tool, msg)); }

	// -h/-help/--help and -version/--version, shared by every tool
	inline void stdFlags(const C8* tool, const String& arg, void (*usage)(std::ostream&)) {
		if(arg == "-h" || arg == "-help" || arg == "--help") {
			usage(std::cout);
			std::exit(0);
		}
		if(arg == "-version" || arg == "--version") {
			printVersion(std::cout, tool);
			std::exit(0);
		}
	}

	// -name <v> and -name=<v>, advances i past a consumed argument
	inline B32 value(const C8* tool, I32 argc, C8** argv, I32& i, const C8* name, String& out) {
		String arg = argv[i];
		U64 n = std::strlen(name);
		if(arg.rfind(name, 0) == 0 && arg.size() > n && arg[n] == '=') {
			out = arg.substr(n + 1);
			return true;
		}
		if(arg != name)
			return false;
		if(++i >= argc)
			die(tool, arg + " expects an argument");
		out = argv[i];
		return true;
	}

	// "" reads stdin
	inline B32 readInput(const C8* tool, const String& path, String& source) {
		std::ifstream file;
		if(!path.empty())
			file.open(path);
		std::istream& in = path.empty() ? std::cin : file;
		if(in && readAll(in, source))
			return true;
		String shownPath = path.empty() ? "<stdin>" : path;
		error(tool, "cannot read '" + shownPath + "'");
		return false;
	}

	inline B32 openOutput(const C8* tool, const String& path, std::ofstream& f, B32 binary = false) {
		f.open(path, binary ? std::ios::binary : std::ios::out);
		if(f)
			return true;
		error(tool, "cannot write '" + path + "'");
		return false;
	}

	inline I32 guardedMain(const C8* tool, I32 (*run)(I32, C8**), I32 argc, C8** argv) {
		try {
			return run(argc, argv);
		} catch(const std::exception& e) {
			std::cerr << tool << ": internal error: " << e.what() << "\n";
		} catch(...) {
			std::cerr << tool << ": internal error\n";
		}
		return 3;
	}
} // namespace rat::cli

#endif
