#include "Host.h"

#include "rat.h"

#include <cstdio>
#include <cstdlib>

namespace rat::cc {
	// portable popen/pclose
	FILE* shellOpen(const char* cmd) {
#if defined(_WIN32)
		return _popen(cmd, "r");
#else
		return popen(cmd, "r");
#endif
	}

	I32 shellClose(FILE* p) {
#if defined(_WIN32)
		return _pclose(p);
#else
		return pclose(p);
#endif
	}

	namespace {
		constexpr U32 kReadBufSize = 4096;

		String captureCmd(const String& cmd) {
			String out;
			FILE* p = shellOpen(cmd.c_str());
			if(!p)
				return out;
			char buf[kReadBufSize];
			U64 n;
			while((n = fread(buf, 1, sizeof(buf), p)) > 0)
				out.append(buf, n);
			shellClose(p);
			return out;
		}
	} // namespace

	const char* nullDevice() {
#if defined(_WIN32)
		return "NUL";
#else
		return "/dev/null";
#endif
	}

	namespace {
		TargetTriple& hostTripleStorage() {
			static TargetTriple triple = [] {
				TargetTriple t;
#if defined(_WIN32)
				t.os = OS::Windows; // native builds default to the host platform
#endif
				const char* env = std::getenv("RATCC_TARGET");
				if(env && *env) {
					String err;
					if(!TargetTriple::parse(env, t, err))
						std::fprintf(stderr, "ratcc: ignoring RATCC_TARGET: %s\n", err.c_str());
				}
				return t;
			}();
			return triple;
		}
	} // namespace

	const TargetTriple& hostTargetTriple() { return hostTripleStorage(); }
	void setHostTargetTriple(const TargetTriple& triple) { hostTripleStorage() = triple; }

	const String& hostCC() {
		static String cc = [] {
			const char* env = std::getenv("CC");
			return String(env && *env ? env : "cc");
		}();
		return cc;
	}

	const String& hostPredefs() {
		static String cache = [] {
			String defs =
					captureCmd(hostCC() + " -std=c11 -dM -E -xc " + nullDevice() + " 2>" + nullDevice()) +
					"\n";
			if(hostTargetTriple().isWindows())
				defs += "#define __LLP64__ 1\n";
			return defs;
		}();
		return cache;
	}

	const List<String>& hostIncludeDirs() {
		static List<String> cache = [] {
			List<String> dirs;
			String v = captureCmd(hostCC() + " -E -v -xc " + String(nullDevice()) + " 2>&1");
			std::istringstream in(v);
			String line;
			B32 inList = false;
			while(std::getline(in, line)) {
				if(line.find("#include <...> search starts here:") != String::npos) {
					inList = true;
					continue;
				}
				if(line.find("End of search list.") != String::npos)
					break;
				if(!inList)
					continue;
				U32 b = 0;
				while(b < line.size() && (line[b] == ' ' || line[b] == '\t'))
					++b;
				U32 e = (U32)line.size();
				while(e > b && (line[e - 1] == '\r' || line[e - 1] == ' '))
					--e;
				if(e > b)
					dirs.push_back(line.substr(b, e - b));
			}
			return dirs;
		}();
		return cache;
	}
} // namespace rat::cc
