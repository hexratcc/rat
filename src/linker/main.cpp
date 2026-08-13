#include "linker.h"

#include "cli.h"
#include "string.h"

using namespace rat;

namespace {
	const C8* kTool = "ratl";

	void usage(std::ostream& os) {
		os << "usage: ratl [options] <input.o>...\n"
					"\n"
					"  -o <file>            output executable (default a.out)\n"
					"  -L<dir>              add a library search directory\n"
					"  -l<name>             link against lib<name>.so (default: c, m)\n"
					"  -e <sym>             program entry symbol passed to libc (default main)\n"
					"  -dynamic-linker <p>  interpreter to request (default /lib64 loader)\n"
					"  -rpath <dir>         add a DT_RUNPATH dir the loader searches at runtime\n"
					"  @<file>              read additional options from a response file\n"
					"  -h, -help            show this help\n"
					"  -version             show version\n"
					"\n"
					"rat:\n"
					"  -target <os>         linux (default) or windows (WIP)\n"
					"\n"
					"other options are ignored for compiler-driver compatibility\n";
	}

	// expand @response files and split -Wl,a,b
	List<String> expandArgs(I32 argc, C8** argv) {
		List<String> out;
		for(I32 i = 1; i < argc; ++i) {
			String a = argv[i], text;
			std::ifstream f(a.rfind('@', 0) == 0 ? a.substr(1) : String());
			if(a.rfind("-Wl,", 0) == 0)
				text = a.substr(4);
			if(text.empty() && !(f && readAll(f, text)))
				out.push_back(std::move(a));
			else
				for(String& t : splitTokens(text))
					out.push_back(std::move(t));
		}
		return out;
	}

	// gnu-ld flags we ignore together with their separate argument
	const String kSkipArg = " --version-script -soname --soname --hash-style -m -z -T -R"
													" --defsym --exclude-libs -plugin --sysroot -a ";
} // namespace

static I32 run(I32 argc, C8** argv) {
	LinkOptions opt;
	opt.output = "a.out";

	List<String> args = expandArgs(argc, argv);
	for(U64 i = 0; i < args.size(); ++i) {
		String arg = args[i];
		auto next = [&](const C8* what) -> String {
			if(++i >= args.size())
				cli::die(kTool, String(what) + " expects an argument");
			return args[i];
		};
		cli::stdFlags(kTool, arg, usage);
		if(arg == "-o")
			opt.output = next("-o");
		else if(arg == "-target") {
			String t = next("-target");
			if(t != "linux" && t != "windows")
				return cli::error(kTool, "unknown -target '" + t + "'");
			opt.target = t == "linux" ? LinkTarget::LinuxX64 : LinkTarget::WindowsX64;
		} else if(arg == "-e" || arg == "--entry")
			opt.entry = next("-e");
		else if(arg == "-dynamic-linker" || arg == "--dynamic-linker" ||
						arg.rfind("--dynamic-linker=", 0) == 0)
			opt.interp = arg.size() > 16 ? arg.substr(17) : next("-dynamic-linker");
		else if(arg == "-rpath" || arg == "--rpath" || arg.rfind("-rpath=", 0) == 0)
			opt.rpaths.push_back(arg.size() > 6 && arg[6] == '=' ? arg.substr(7) : next("-rpath"));
		else if(arg == "--library-path" || arg.rfind("-L", 0) == 0)
			opt.libPaths.push_back(arg.size() > 2 && arg[1] == 'L' ? arg.substr(2) : next("-L"));
		else if(arg == "--library" || arg.rfind("-l", 0) == 0)
			opt.libs.push_back(arg.size() > 2 && arg[1] == 'l' ? arg.substr(2) : next("-l"));
		else if(kSkipArg.find(" " + arg + " ") != String::npos)
			next(arg.c_str()); // ignore flag + arg
		else if(arg.size() > 1 && arg[0] == '-')
			continue; // ignore unknown driver flags
		else
			opt.inputs.push_back(arg);
	}

	if(opt.inputs.empty())
		return cli::error(kTool, "no input files");

	String err;
	if(!link(opt, err))
		return std::cerr << kTool << ": " << err << "\n", 1;
	return 0;
}

I32 main(I32 argc, C8** argv) { return cli::guardedMain(kTool, run, argc, argv); }
