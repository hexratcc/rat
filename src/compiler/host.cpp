#include "host.h"

#include <cstdio>
#include <cstdlib>

namespace rat::cc {
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
} // namespace rat::cc
