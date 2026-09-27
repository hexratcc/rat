#include "emit/emit_state.h"

namespace rat::cc {
	void Scopes::push() { marks.push_back((U32)undo.size()); }

	void Scopes::pop() {
		U32 mark = marks.back();
		marks.pop_back();
		while(undo.size() > mark) {
			Undo& u = undo.back();
			auto it = table.find(*u.name);
			if(u.hadPrev)
				it->second = u.prev;
			else
				table.erase(it);
			undo.pop_back();
		}
	}

	void Scopes::declare(const String& name, Local local) {
		auto [it, inserted] = table.try_emplace(name, local);
		if(inserted) {
			undo.push_back({&it->first, Local{}, false});
		} else {
			undo.push_back({&it->first, it->second, true});
			it->second = local;
		}
	}

	B32 Scopes::lookup(const String& name, Local& out) const {
		auto found = table.find(name);
		if(found == table.end())
			return false;
		out = found->second;
		return true;
	}

	void Scopes::clear() {
		table.clear();
		undo.clear();
		marks.clear();
	}

	void FunctionState::reset() {
		returnType = {};
		sretSlot = nullptr;
		sp = nullptr;
		labelSp.clear();
		sawAlloca = false;
		addrTaken.clear();
		labelBlocks.clear();
		loops.clear();
		switches.clear();
		scopes.clear();
	}

	void Diagnostics::fail(const String& m) {
		if(failed)
			return;
		message =
				m + " [@" + std::to_string(offset) + (function.empty() ? "" : " in " + function) + "]";
		failed = true;
	}

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
