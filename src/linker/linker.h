#ifndef RAT_RATL_LINKER_H
#define RAT_RATL_LINKER_H

#include "core.h"

namespace rat {
	enum class LinkTarget {
		LinuxX64,
		WindowsX64 // TODO
	};

	struct LinkOptions {
		List<String> inputs;
		String output;
		List<String> libPaths; // -L dirs
		List<String> libs;		 // -l names
		String interp;
		List<String> rpaths;	 // DT_RUNPATH dirs
		String entry = "main";
		LinkTarget target = LinkTarget::LinuxX64;
	};

	B32 link(const LinkOptions& opt, String& err);
} // namespace rat

#endif
