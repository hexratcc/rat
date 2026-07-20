#ifndef RAT_SUPPORT_PASSREGISTRY_H
#define RAT_SUPPORT_PASSREGISTRY_H

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

		static void registerAll(PassRegistry& r);

		void add(String name, String description, Factory make);

		template <typename P> void add(String name, String description) {
			if constexpr(std::is_constructible_v<P, std::ostream&>)
				add(std::move(name), std::move(description), [](std::ostream& os) -> UniquePtr<Pass> {
					return std::make_unique<P>(os);
				});
			else
				add(std::move(name), std::move(description), [](std::ostream&) -> UniquePtr<Pass> {
					return std::make_unique<P>();
				});
		}

		UniquePtr<Pass> create(const String& name, std::ostream& out) const;

		const List<Entry>& entries() const { return items; }
	private:
		const Entry* find(const String& name) const;

		List<Entry> items;
	};

	PassRegistry& passRegistry();
	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err);
	List<String> defaultOptPipeline();
} // namespace rat

#endif
