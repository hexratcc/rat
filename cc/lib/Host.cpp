#include "Host.h"

#include "rat.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

	namespace {
		struct HostConfig {
			String predefs;
			List<String> includeDirs;
		};

		String hostCachePath() {
			if(const char* p = std::getenv("RATCC_HOST_CACHE"))
				return *p ? String(p) : String(); // empty value disables the cache
			String dir;
			if(const char* x = std::getenv("XDG_CACHE_HOME"))
				dir = x;
			else if(const char* h = std::getenv("HOME"))
				dir = String(h) + "/.cache";
			else
				return String();
			return dir + "/ratcc-host.cache";
		}

		String hostCacheKey() {
			return "ratcc-host-v1 cc=" + hostCC();
		}

		B32 loadHostCache(const String& path, HostConfig& out) {
			std::ifstream in(path, std::ios::binary);
			if(!in)
				return false;
			struct stat st;
			if(stat(path.c_str(), &st) == 0) {
				// refuse stale caches so toolchain upgrades are picked up
				std::time_t now = std::time(nullptr);
				if(now - st.st_mtime > 24 * 60 * 60)
					return false;
			}
			String line;
			if(!std::getline(in, line) || line != hostCacheKey())
				return false;
			U32 ndirs = 0;
			if(!(in >> ndirs) || ndirs > 256)
				return false;
			in.ignore(1);
			for(U32 i = 0; i < ndirs; ++i) {
				if(!std::getline(in, line))
					return false;
				out.includeDirs.push_back(line);
			}
			std::ostringstream rest;
			rest << in.rdbuf();
			out.predefs = rest.str();
			return true;
		}

		void saveHostCache(const String& path, const HostConfig& cfg) {
			if(path.empty())
				return;
			// only cache answers that look like a working toolchain, and write
			// through a per-process temp so concurrent compiles cannot collide
			if(cfg.predefs.find("#define") == String::npos)
				return;
			String tmp = path + ".tmp" + std::to_string((U64)getpid());
			{
				std::ofstream outF(tmp, std::ios::binary | std::ios::trunc);
				if(!outF)
					return;
				outF << hostCacheKey() << "\n" << cfg.includeDirs.size() << "\n";
				for(const String& d : cfg.includeDirs)
					outF << d << "\n";
				outF << cfg.predefs;
			}
			std::rename(tmp.c_str(), path.c_str());
		}

		const HostConfig& hostConfig() {
			static HostConfig cfg = [] {
				HostConfig c;
				String path = hostCachePath();
				if(!path.empty() && loadHostCache(path, c))
					return c;
				c = HostConfig();
				c.predefs =
						captureCmd(hostCC() + " -std=c11 -dM -E -xc " + nullDevice() + " 2>" + nullDevice()) +
						"\n";
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
						c.includeDirs.push_back(line.substr(b, e - b));
				}
				saveHostCache(path, c);
				return c;
			}();
			return cfg;
		}
	} // namespace

	const String& hostPredefs() {
		static String cache = [] {
			String defs = hostConfig().predefs;
			if(hostTargetTriple().isWindows())
				defs += "#define __LLP64__ 1\n";
			return defs;
		}();
		return cache;
	}

	const List<String>& hostIncludeDirs() { return hostConfig().includeDirs; }
} // namespace rat::cc
