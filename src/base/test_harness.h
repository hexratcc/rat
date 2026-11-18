#ifndef RAT_TESTKIT_TESTHARNESS_H
#define RAT_TESTKIT_TESTHARNESS_H

#include "core.h"

#include <atomic>
#include <mutex>

namespace rat {
	struct TestSuiteSpec {
		const C8* tool;															 // program name
		const C8* extension;												 // case file suffix
		List<String> dirCandidates;									 // searched when no paths are given
		B32 (*run)(const String& path, String& err); // executes a single case
	};

	namespace detail {
		// shared by the worker threads
		struct SuiteRun {
			std::atomic<U32> next{0};
			std::mutex ioMtx;
			U32 passed = 0;
			List<String> failures;
			void work(const TestSuiteSpec& spec, const List<String>& cases);
		};

		void collectCases(const String& dir, const C8* ext, List<String>& out);
		String findDir(const List<String>& candidates);
	} // namespace detail

	I32 runTestSuite(I32 argc, C8** argv, const TestSuiteSpec& spec);
} // namespace rat

#endif
