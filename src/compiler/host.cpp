#include "host.h"

#include <cstdio>
#include <cstdlib>

namespace rat::cc {
	namespace detail {
		TargetTriple defaultTriple() {
			TargetTriple t;
#if defined(_WIN32)
			t.os = OS::Windows; // native builds default to the host platform
#endif
			const C8* env = std::getenv("RATCC_TARGET");
			if(env && *env) {
				String err;
				if(!TargetTriple::parse(env, t, err))
					std::fprintf(stderr, "ratcc: ignoring RATCC_TARGET: %s\n", err.c_str());
			}
			return t;
		}

		TargetTriple& hostTripleStorage() {
			static TargetTriple triple = defaultTriple();
			return triple;
		}
	} // namespace detail

	const TargetTriple& hostTargetTriple() { return detail::hostTripleStorage(); }
	void setHostTargetTriple(const TargetTriple& triple) { detail::hostTripleStorage() = triple; }
} // namespace rat::cc
