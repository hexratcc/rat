#ifndef RAT_SUPPORT_PASSREGISTRY_H
#define RAT_SUPPORT_PASSREGISTRY_H

#include "core.h"
#include "pass/pass_manager.h"

namespace rat {
	UniquePtr<Pass> createPass(const String& name, std::ostream& out);
	UniquePtr<MachinePass> createMachinePass(const String& name, std::ostream& out);

	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err);
	void listPasses(std::ostream& os, B32 withMachine);
	List<String> defaultOptPipeline();
} // namespace rat

#endif
