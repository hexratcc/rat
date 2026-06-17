#ifndef RAT_PASS_PASSREGISTRY_H
#define RAT_PASS_PASSREGISTRY_H

#include "Core.h"
#include "Pass/PassManager.h"

namespace rat {
	struct Pass;

	struct PassRegistry {
		using Factory = UniquePtr<Pass> (*)(std::ostream& out);

		struct Entry {
			String name;
			String description;
			Factory make;
		};

		void add(String name, String description, Factory make);

		const Entry* find(const String& name) const;
		UniquePtr<Pass> create(const String& name, std::ostream& out) const;

		const List<Entry>& entries() const { return items; }

	private:
		List<Entry> items;
	};

	PassRegistry& passRegistry();
	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out,
										String& err);
} // namespace rat

#endif
