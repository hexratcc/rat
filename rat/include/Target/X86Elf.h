#ifndef RAT_TARGET_X86ELF_H
#define RAT_TARGET_X86ELF_H

#include "Core.h"

#include "Target/ObjectFile.h"

namespace rat {
	// ELF64 relocatable object writer
	struct ElfObject final : ObjectFile {
		void write(std::ostream& os) override;
	};
} // namespace rat

#endif
