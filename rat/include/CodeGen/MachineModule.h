#ifndef RAT_CODEGEN_MACHINEMODULE_H
#define RAT_CODEGEN_MACHINEMODULE_H

#include "Core.h"

#include "CodeGen/MachineFunction.h"

namespace rat {
	struct MachineModule {
		Map<const Function*, MachineFunc> funcs;

		MachineFunc& get(const Function* f) {
			auto it = funcs.find(f);
			if(it != funcs.end())
				return it->second;
			MachineFunc& mf = funcs[f];
			mf.src = f;
			return mf;
		}
	};
} // namespace rat

#endif
