#ifndef RAT_TESTKIT_TESTHARNESS_H
#define RAT_TESTKIT_TESTHARNESS_H

#include "core.h"

namespace rat {
	namespace detail {
		B32 hasSuffix(const String& s, const C8* suffix);
		void collectCases(const String& dir, const C8* ext, List<String>& out);
		String findDir(const List<String>& candidates);
	} // namespace detail

	using CaseRunner = Delegate<B32(const String& path, String& err)>;

	struct TestSuiteSpec {
		const C8* tool;							// program name
		const C8* extension;				// case file suffix
		List<String> dirCandidates; // searched when no paths are given
		CaseRunner run;							// executes a single case
	};

	I32 runTestSuite(I32 argc, C8** argv, const TestSuiteSpec& spec);
} // namespace rat

#endif
