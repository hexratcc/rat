#include "emit/emit.h"
#include "lex/token_stream.h"

#include "git_hash.h"

#include <filesystem>

namespace rat::cc {
	void Emitter::enableDebug(const TokenStream& ts, B32 describeVars) {
		UniquePtr<DebugInfo> info = std::make_unique<DebugInfo>();
		info->producer = String("ratcc ") + GIT_HASH;
		std::error_code ec;
		info->compDir = std::filesystem::current_path(ec).string();
		info->files.push_back(ts.file());
		dbg.toks = &ts;
		dbg.info = info.get();
		dbg.describeVars = describeVars;
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

	DebugType* Emitter::debugType(CType t) {
		using Kind = DebugType::Kind;
		if(t.ptr > 0)
			return debugWrap(Kind::Pointer, debugType(pointee(t)));
		if(isArrayType(t))
			return debugObjectType(t.array->elem, true, t.array->count);
		if(t.func) {
			DebugType* f = dbg.info->makeType(Kind::Function, "", 0);
			f->elem = debugType(t.func->ret);
			for(const Param& p : t.func->params)
				f->params.push_back(debugType(p.type));
			f->variadic = t.func->isVarArgs;
			return f;
		}
		if(isVoidType(t))
			return nullptr;
		if(isAggregate(t) && !t.isComplex())
			return debugRecord(t.strukt);
		return debugBase(t);
	}

	DebugType* Emitter::debugObjectType(CType t, B32 isArray, U64 count) {
		DebugType* elem = debugType(t);
		if(!isArray)
			return elem;
		DebugType* a = dbg.info->makeType(DebugType::Kind::Array, "", (U32)(count * byteSize(t)));
		a->elem = elem;
		a->count = count;
		return a;
	}

	// clang's spelling, typeName folds signed char and the long variants into others
	DebugType* Emitter::debugBase(CType t) {
		using Kind = DebugType::Kind;
		String sign = t.isUnsigned() ? "unsigned " : "";
		CType plain = t;
		plain.quals = plain.vquals = plain.rquals = 0;
		String name = typeName(plain);
		if(t.bits == 8 && !t.isFloat() && !t.isPlainChar())
			name = sign.empty() ? "signed char" : "unsigned char";
		else if(t.isLongLong())
			name = sign + "long long";
		else if(t.isLong() && !t.isFloat())
			name = sign + "long";
		DebugType*& b = dbg.bases[name];
		if(b)
			return b;
		Kind kind = t.isUnsigned() ? Kind::Unsigned : Kind::Signed;
		if(t.bits == 8)
			kind = t.isUnsigned() ? Kind::UnsignedChar : Kind::SignedChar;
		if(t.bits == 1)
			kind = Kind::Bool;
		if(t.isFloat())
			kind = t.isComplex() ? Kind::ComplexFloat : Kind::Float;
		b = dbg.info->makeType(kind, name, (U32)byteSize(t));
		return b;
	}

	DebugType* Emitter::debugRecord(const StructType* st) {
		DebugType::Kind kind = st->isUnion ? DebugType::Kind::Union : DebugType::Kind::Struct;
		DebugType*& rec = dbg.types[{kind, st}];
		if(rec)
			return rec;
		rec = dbg.info->makeType(kind, st->tag, (U32)st->size);
		rec->incomplete = !st->complete;
		// anonymous struct members are already lifted into the parent at their final offsets
		for(const Field& f : st->fields) {
			if(!f.name)
				continue;
			DebugType* type = debugObjectType(f.type, f.isArray(), f.count);
			U32 bits = f.isBitfield() ? f.bitWidth : 0;
			rec->members.push_back({*f.name, type, (U32)f.offset, bits, f.bitOffset});
		}
		return rec;
	}

	DebugType* Emitter::debugWrap(DebugType::Kind kind, DebugType* elem) {
		DebugType*& w = dbg.types[{kind, elem}];
		if(!w) {
			w = dbg.info->makeType(kind, "", kind == DebugType::Kind::Pointer ? lay.ptrBytes : 0);
			w->elem = elem;
		}
		return w;
	}

	void Emitter::debugFunction(Function& fn, const FuncDef* def) {
		if(!dbg.info)
			return;
		dbg.info->funcs.push_back(std::make_unique<DebugFunc>());
		DebugFunc& df = *dbg.info->funcs.back();
		df.name = def->name;
		df.decl = debugLoc(def->offset);
		df.ret = dbg.describeVars ? debugType(def->retType) : nullptr;
		df.external = !def->isStatic;
		df.variadic = def->isVarArgs;
		fn.setDebug(std::make_unique<DebugBody>());
		fn.getDebug()->func = &df;
	}

	void Emitter::debugSetLoc(Function& fn, U32 tok) {
		if(dbg.info)
			fn.setLoc(fn.getDebug()->addLoc(debugLoc(tok)));
	}

	void Emitter::debugGlobal(const Declarator& d) {
		if(!dbg.describeVars)
			return;
		const GlobalVariable& gv = syms.globals[*d.name];
		DebugType* type = debugObjectType(gv.type, gv.isArray, gv.count);
		dbg.info->globals.push_back({*d.name, type, debugLoc(d.offset), *d.name});
	}
} // namespace rat::cc
