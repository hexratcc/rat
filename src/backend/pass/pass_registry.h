#ifndef RAT_SUPPORT_PASSREGISTRY_H
#define RAT_SUPPORT_PASSREGISTRY_H

#include "core.h"
#include "pass/pass_manager.h"

namespace rat {
	struct Pass;

	struct PassRegistry {
		using Factory = UniquePtr<Pass> (*)(std::ostream& out);

		struct Entry {
			const C8* name;
			const C8* description;
			Factory make;
		};

		struct EntryTable {
			const Entry* items;
			U32 count;

			const Entry* begin() const { return items; }
			const Entry* end() const { return items + count; }
		};

		UniquePtr<Pass> create(const String& name, std::ostream& out) const;

		static EntryTable entries();
	private:
		static const Entry* find(const String& name);
	};

	const PassRegistry& passRegistry();
	B32 buildPipeline(PassManager& pm, const String& spec, std::ostream& out, String& err);

	List<UniquePtr<Pass>> makeDefaultOptPasses();
	List<String> defaultOptPipeline();
} // namespace rat

#endif
