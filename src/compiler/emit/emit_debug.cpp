#include "emit/emit.h"
#include "lex/token_stream.h"

#include "git_hash.h"

#include <filesystem>

namespace rat::cc {
	void Emitter::enableDebug(const TokenStream& ts) {
		UniquePtr<DebugInfo> info = std::make_unique<DebugInfo>();
		info->producer = String("ratcc ") + GIT_HASH;
		std::error_code ec;
		info->compDir = std::filesystem::current_path(ec).string();
		info->files.push_back(ts.file());
		dbg.toks = &ts;
		dbg.info = info.get();
		mod.setDebugInfo(std::move(info));
	}

	SrcLoc Emitter::debugLoc(U32 tok) {
		const Token& t = dbg.toks->toks[tok];
		auto [it, fresh] = dbg.files.try_emplace(t.file, 0);
		List<String>& files = dbg.info->files;
		const String& name = dbg.toks->files[t.file];
		if(fresh && name != files[0]) { // names are unique, only the primary is already there
			it->second = (U32)files.size();
			files.push_back(name);
		}
		return {it->second, t.line, t.col};
	}

	void Emitter::debugFunction(Function& fn, const FuncDef* def) {
		if(!dbg.info)
			return;
		dbg.info->funcs.push_back(std::make_unique<DebugFunc>());
		DebugFunc& df = *dbg.info->funcs.back();
		df.name = def->name;
		df.decl = debugLoc(def->offset);
		df.external = !def->isStatic;
		df.variadic = def->isVarArgs;
		fn.setDebug(std::make_unique<DebugBody>());
		fn.getDebug()->func = &df;
	}

	void Emitter::debugSetLoc(Function& fn, U32 tok) {
		if(dbg.info)
			fn.setLoc(fn.getDebug()->addLoc(debugLoc(tok)));
	}
} // namespace rat::cc
