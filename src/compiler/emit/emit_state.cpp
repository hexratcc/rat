#include "emit/emit_state.h"

namespace rat::cc {
	Failed Diagnostics::fail(const String& m) {
		if(failed)
			return {};
		message = m + " [@" + std::to_string(offset);
		if(!function.empty())
			message += " in " + function;
		message += "]";
		failed = true;
		return {};
	}

	DryRunDiag::DryRunDiag(Diagnostics& current)
	: diag(current),
		saved(std::exchange(current, {})) {}

	DryRunDiag::~DryRunDiag() { diag = std::move(saved); }

	const String& Symbols::resolveAlias(const String& name) const {
		auto it = aliases.find(name);
		return it == aliases.end() ? name : it->second;
	}

	String GlobalData::nextName(const C8* prefix) { return prefix + std::to_string(nameCounter++); }

	void GlobalData::setReloc(U32 off, const String& sym, I64 add) {
		for(Reloc& r : relocs)
			if(r.offset == off) {
				r = Reloc{off, sym, add};
				return;
			}
		relocs.push_back(Reloc{off, sym, add});
	}
} // namespace rat::cc
