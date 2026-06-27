#ifndef RAT_PASS_EMIT_X86EMITTER_H
#define RAT_PASS_EMIT_X86EMITTER_H

#include "CodeGen/RegAlloc.h"
#include "Core.h"
#include "Support/Pass.h"

namespace rat {
	struct ElfObject;
	struct Function;
	struct Global;
	struct MachineModule;
	struct Module;
	struct TargetInfo;

	RegAllocHooks x86RegAllocHooks();

	struct X86LowerPass : MachinePass {
		const C8* name() const override { return "x86-lower"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		U32 runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target);
	};

	struct X86EncodePass : MachinePass {
		explicit X86EncodePass(std::ostream& os)
		: os(&os) {}

		const C8* name() const override { return "x86-encode"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		void emitGlobal(ElfObject& elf, const Module& mod, const Global* g);

		std::ostream* os;
	};
} // namespace rat

#endif
