#ifndef RAT_TARGET_X86COFF_H
#define RAT_TARGET_X86COFF_H

#include "Core.h"

#include "Target/ObjectFile.h"

namespace rat {
	// COFF relocatable object writer
	struct CoffObject final : ObjectFile {
		void write(std::ostream& os) override;
	};
} // namespace rat

#endif
