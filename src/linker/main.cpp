#include "linker.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace rat;

namespace {
	void usage(std::ostream& os, const char* prog) {
		os << "usage: " << prog << " [options] <input.o>...\n"
			 << "  -o <file>       output executable (default a.out)\n"
			 << "  -L <dir>        add a library search directory\n"
			 << "  -l <name>       link against lib<name>.so (default: c, m)\n"
			 << "  -e <sym>        program entry symbol passed to libc (default main)\n"
			 << "  -dynamic-linker <p>  interpreter to request (default /lib64 loader)\n"
			 << "  -rpath <dir>    add a DT_RUNPATH dir the loader searches at runtime\n"
			 << "  -target <os>    linux (default) or windows (not yet implemented)\n"
			 << "  -bench <n>      link n times in-process and report us/link, then exit\n"
			 << "  -h, --help      show this help\n"
			 << "\n";
	}

	int runBench(const LinkOptions& opt, U32 reps) {
		String err;
		if(!link(opt, err)) { // warm once
			std::cerr << "ratl: " << err << "\n";
			return 1;
		}
		auto t0 = std::chrono::steady_clock::now();
		for(U32 i = 0; i < reps; ++i) {
			if(!link(opt, err)) {
				std::cerr << "ratl: " << err << "\n";
				return 1;
			}
		}
		auto t1 = std::chrono::steady_clock::now();
		F64 us = std::chrono::duration<F64, std::micro>(t1 - t0).count() / (F64)reps;
		std::printf(
				"ratl in-process: %.1f us/link over %u reps (%.0f links/sec)\n", us, reps, 1e6 / us);
		return 0;
	}

	// expand @response files and split -Wl,a,b
	void tokenize(const String& s, List<String>& out) {
		String cur;
		for(char c : s) {
			if(c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				if(!cur.empty()) {
					out.push_back(cur);
					cur.clear();
				}
			} else
				cur += c;
		}
		if(!cur.empty())
			out.push_back(cur);
	}

	void expandArgs(int argc, char** argv, List<String>& out) {
		for(int i = 1; i < argc; ++i) {
			String a = argv[i];
			if(!a.empty() && a[0] == '@') {
				std::ifstream f(a.substr(1));
				if(f) {
					std::stringstream ss;
					ss << f.rdbuf();
					List<String> toks;
					tokenize(ss.str(), toks);
					for(String& t : toks)
						out.push_back(std::move(t));
					continue;
				}
			}
			if(a.rfind("-Wl,", 0) == 0) {
				String rest = a.substr(4), cur;
				for(char c : rest) {
					if(c == ',') {
						out.push_back(cur);
						cur.clear();
					} else
						cur += c;
				}
				out.push_back(cur);
				continue;
			}
			out.push_back(std::move(a));
		}
	}
} // namespace

int main(int argc, char** argv) {
	LinkOptions opt;
	opt.output = "a.out";
	U32 benchReps = 0;

	List<String> args;
	expandArgs(argc, argv, args);

	// flags whose separate arg must be skipped
	auto takesArg = [](const String& a) {
		static const char* v[] = {"--version-script",
															"-soname",
															"--soname",
															"--hash-style",
															"-m",
															"-z",
															"-T",
															"-R",
															"--defsym",
															"--exclude-libs",
															"-plugin",
															"--sysroot",
															"-a"};
		for(const char* s : v)
			if(a == s)
				return true;
		return false;
	};

	for(size_t i = 0; i < args.size(); ++i) {
		String arg = args[i];
		auto next = [&](const char* what) -> String {
			if(++i >= args.size()) {
				std::cerr << "ratl: " << what << " expects an argument\n";
				std::exit(2);
			}
			return args[i];
		};
		if(arg == "-h" || arg == "--help") {
			usage(std::cout, argv[0]);
			return 0;
		} else if(arg == "-o") {
			opt.output = next("-o");
		} else if(arg == "-bench") {
			benchReps = (U32)std::strtoul(next("-bench").c_str(), nullptr, 10);
		} else if(arg == "-target") {
			String t = next("-target");
			if(t == "linux")
				opt.target = LinkTarget::LinuxX64;
			else if(t == "windows")
				opt.target = LinkTarget::WindowsX64;
			else {
				std::cerr << "ratl: unknown -target '" << t << "'\n";
				return 2;
			}
		} else if(arg == "-e" || arg == "--entry") {
			opt.entry = next("-e");
		} else if(arg == "-dynamic-linker" || arg == "--dynamic-linker") {
			opt.interp = next("-dynamic-linker");
		} else if(arg.rfind("--dynamic-linker=", 0) == 0) {
			opt.interp = arg.substr(17);
		} else if(arg == "-rpath" || arg == "--rpath") {
			opt.rpaths.push_back(next("-rpath"));
		} else if(arg.rfind("-rpath=", 0) == 0) {
			opt.rpaths.push_back(arg.substr(7));
		} else if(arg == "-L" || arg == "--library-path") {
			opt.libPaths.push_back(next("-L"));
		} else if(arg.rfind("-L", 0) == 0) {
			opt.libPaths.push_back(arg.substr(2));
		} else if(arg == "-l" || arg == "--library") {
			opt.libs.push_back(next("-l"));
		} else if(arg.rfind("-l", 0) == 0) {
			opt.libs.push_back(arg.substr(2));
		} else if(takesArg(arg)) {
			next(arg.c_str()); // ignore flag + arg
		} else if(!arg.empty() && arg[0] == '-') {
			continue;
		} else {
			opt.inputs.push_back(arg);
		}
	}

	if(opt.inputs.empty()) {
		usage(std::cerr, argv[0]);
		return 2;
	}

	if(benchReps) {
		opt.executable = false; // skip chmod in hot loop
		return runBench(opt, benchReps);
	}

	String err;
	if(!link(opt, err)) {
		std::cerr << "ratl: " << err << "\n";
		return 1;
	}
	return 0;
}
