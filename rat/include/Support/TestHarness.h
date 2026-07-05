#ifndef RAT_SUPPORT_TESTHARNESS_H
#define RAT_SUPPORT_TESTHARNESS_H

#include "Core.h"

namespace rat {
	using CaseRunner = Delegate<B32(const String& path, String& err)>;

	struct TestSuiteSpec {
		const char* tool;						// program name
		const char* extension;			// case file suffix
		List<String> dirCandidates; // searched when no paths are given
		CaseRunner run;							// executes a single case
		Delegate<void()> prewarm;		// runs once before threads spawn
	};

	I32 runTestSuite(I32 argc, char** argv, const TestSuiteSpec& spec);
} // namespace rat

#endif
