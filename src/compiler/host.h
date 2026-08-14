#ifndef RAT_CC_HOST_H
#define RAT_CC_HOST_H

#include "core.h"

#include "target/target.h"

namespace rat::cc {
	namespace detail {
		TargetTriple& hostTripleStorage();
	} // namespace detail

	const TargetTriple& hostTargetTriple();
	void setHostTargetTriple(const TargetTriple& triple);
} // namespace rat::cc

#endif
