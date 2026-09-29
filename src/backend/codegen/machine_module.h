#ifndef RAT_CODEGEN_MACHINEMODULE_H
#define RAT_CODEGEN_MACHINEMODULE_H

#include "core.h"

#include "codegen/machine_function.h"

namespace rat {
	struct MachineModule {
		Map<const Function*, MachineFunc> funcs;

		MachineFunc& get(const Function* f) { return funcs[f]; }
	};
} // namespace rat

#endif
