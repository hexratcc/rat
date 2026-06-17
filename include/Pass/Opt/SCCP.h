#ifndef RAT_PASS_OPT_SCCP_H
#define RAT_PASS_OPT_SCCP_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 sccp(Function& fn);

	struct SCCPPass : FunctionPass {
		const char* name() const override;
		U32 runOnFunction(Function& fn) override;
	};
} // namespace rat

#endif
