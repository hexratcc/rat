#ifndef RAT_SUPPORT_PASSREGISTRY_H
#define RAT_SUPPORT_PASSREGISTRY_H

#include "Core.h"
#include "Support/PassManager.h"

namespace rat {
	struct Pass;

	struct PassRegistry {
		using Factory = UniquePtr<Pass> (*)(std::ostream& out);

		struct Entry {
			String name;
			String description;
			Factory make;
		};

		static void registerAll(PassRegistry& r);

		void add(String name, String description, Factory make);

		UniquePtr<Pass> create(const String& name, std::ostream& out) const;

		const List<Entry>& entries() const { return items; }
	private:
		const Entry* find(const String& name) const;

		List<Entry> items;
	};

	PassRegistry& passRegistry();
	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err);
} // namespace rat

#endif
