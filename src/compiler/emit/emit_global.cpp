#include "emit/emit.h"

namespace rat::cc {
	String Emitter::internString(const Expr* e) {
		const String& bytes = *e->str.bytes;
		U32 cw = e->str.charSize;
		String key = std::to_string(cw) + ":" + bytes;
		auto it = data.strPool.find(key);
		if(it != data.strPool.end())
			return it->second;
		String name = data.nextName("__ratcc_str");
		List<U8> init;
		init.reserve(bytes.size() + cw);
		for(C8 c : bytes)
			init.push_back((U8)c);
		for(U32 i = 0; i < cw; ++i)
			init.push_back(0);
		Global* g = mod.createGlobal(name, byteArrayType((U32)init.size()), true, std::move(init));
		g->setLinkage(Global::Linkage::Internal);
		data.strPool.emplace(std::move(key), name);
		return name;
	}

	Node* Emitter::floatLit(Function& fn, CType t, F80 v) {
		if(t.bits != 128 || (F80)(F64)v == v)
			return fn.constFloat(irType(t), (F64)v);
		List<U8> init;
		encodeFloatBytes(t, v, init);
		String name = data.nextName("__ratcc_ld");
		Global* g = mod.createGlobal(name, irType(t), true, std::move(init));
		g->setLinkage(Global::Linkage::Internal);
		return fn.load(irType(t), fn.global(name));
	}

	B32 Emitter::internCompoundLiteral(const Expr* e, String& outSym) {
		CType ty = e->compound.type;
		const Expr* init = e->compound.init;
		List<Reloc> saved;
		saved.swap(data.relocs);
		B32 ok = true;
		String name = data.nextName("__ratcc_cl");

		U32 total = 0;
		I64 count = 0;
		if(e->compound.isArray) {
			if(e->compound.arrayLen)
				ok = evalConst(e->compound.arrayLen, count) && count > 0;
			else if(init->kind == ExprKind::StrLit)
				count = (I64)init->str.bytes->size() + 1;
			else if(init->kind == ExprKind::InitList)
				count = (I64)arrayInitOuterExtent(ty, init);
			ok = ok && count > 0;
			total = (U32)count * byteSize(ty);
		} else if(isStruct(ty)) {
			total = ty.strukt->size;
		} else {
			total = byteSize(ty);
		}

		if(ok) {
			List<U8> img(total, 0);
			ImageSink sink(*this, img);
			if(e->compound.isArray && init->kind == ExprKind::StrLit)
				ok = sink.charArray(0, ty, (U32)count, init);
			else if(e->compound.isArray)
				ok = initArrayInit(sink, 0, ty, (U32)count, init);
			else if(isStruct(ty))
				ok = initStructInit(sink, 0, ty.strukt, init);
			else
				ok = sink.scalar(0, ty, init);
			if(ok) {
				Global* g = mod.createGlobal(
						name, byteArrayType(total), false, std::move(img), std::move(data.relocs));
				g->setLinkage(Global::Linkage::Internal);
			}
		}

		data.relocs.swap(saved);
		if(!ok) {
			diag.fail("invalid file-scope compound literal initializer");
			return false;
		}
		syms.globals[name] = GlobalVar{ty, e->compound.isArray, 0};
		outSym = name;
		return true;
	}

	void Emitter::bindGlobal(const Declarator& d, const String& sym, Function* fn, B32 arr, U32 n) {
		if(fn) {
			Local loc = Local::mem(fn->global(sym), d.type);
			loc.isArray = arr;
			loc.count = n;
			loc.staticSym = arena.make<String>(sym);
			func.scopes.declare(*d.name, loc);
		} else {
			syms.globals[*d.name] = GlobalVar{d.type, arr, n};
		}
	}

	void Emitter::defineGlobal(const Declarator& d, const String& sym, Type* ty, List<U8>&& img) {
		Global* g = mod.createGlobal(sym, ty, false, std::move(img), std::move(data.relocs));
		if(d.isStatic)
			g->setLinkage(Global::Linkage::Internal);
		g->setAlign(d.align);
	}

	B32 Emitter::validateGlobalArrayLen(const Declarator& d, I64& count, B32& haveLen) {
		haveLen = d.arrayLen != nullptr;
		count = 0;
		if(haveLen && (!evalConst(d.arrayLen, count) || count <= 0)) {
			failArrayCount();
			return false;
		}
		return true;
	}

	B32 Emitter::registerGlobalAggArray(const Declarator& d, const String& sym, Function* fn) {
		B32 haveLen;
		I64 count;
		if(!validateGlobalArrayLen(d, count, haveLen))
			return false;
		if(!haveLen) {
			if(!d.init || d.init->kind != ExprKind::InitList) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			count = (I64)initArrayCount(d.type, d.init);
		}
		U32 total = (U32)count * byteSize(d.type);
		List<U8> img;
		if(d.init) {
			img.assign(total, 0);
			ImageSink sink(*this, img);
			U32 pos = 0;
			if(isStruct(d.type) && initListIsFlat(d.init)) {
				if(!initFlatArray(sink, 0, d.type, (U32)count, {d.init->args, pos}))
					return false;
			} else if(!initArrayInit(sink, 0, d.type, (U32)count, d.init))
				return false;
		}
		defineGlobal(d, sym, byteArrayType(total), std::move(img));
		bindGlobal(d, sym, fn, true, (U32)count);
		return true;
	}

	B32 Emitter::registerGlobalArrayOfScalar(const Declarator& d, const String& sym, Function* fn) {
		U32 elemSize = byteSize(d.type);
		B32 haveLen;
		I64 count;
		if(!validateGlobalArrayLen(d, count, haveLen))
			return false;

		List<U8> init;

		if(d.init && d.init->kind == ExprKind::StrLit) {
			U32 charWidth = d.init->str.isWide ? d.init->str.charSize : 1u;
			if(d.type.ptr != 0 || d.type.bits != charWidth * 8) {
				failStringNeedsCharArray();
				return false;
			}
			const String& bytes = *d.init->str.bytes;
			I64 nchars = (I64)bytes.size() / (I64)charWidth;
			if(!haveLen)
				count = nchars + 1;
			init.assign((U32)count * elemSize, 0);
			for(I64 i = 0; i < nchars && i < count; ++i)
				for(U32 k = 0; k < charWidth; ++k)
					init[(U32)(i * elemSize) + k] = (U8)bytes[(U32)(i * charWidth + k)];
		} else if(d.init && d.init->kind == ExprKind::InitList) {
			const List<Expr*>& els = d.init->args;
			List<I64> idx;
			if(!resolveArrayIndices(d.init, haveLen, count, idx))
				return false;
			init.assign((U32)count * elemSize, 0);
			ImageSink sink(*this, init);
			for(U32 i = 0; i < els.size(); ++i) {
				U32 base = (U32)idx[i] * elemSize;
				if(!sink.scalar(base, d.type, els[i]))
					return false;
			}
		} else if(d.init) {
			diag.fail("invalid initializer for an array");
			return false;
		} else {
			if(!haveLen) {
				failArrayUnknownSize(*d.name);
				return false;
			}
		}

		defineGlobal(d, sym, mod.getArray(irType(d.type), (U32)count), std::move(init));
		bindGlobal(d, sym, fn, true, (U32)count);
		return true;
	}

	B32 Emitter::registerGlobal(const Declarator& d, const String& sym, Function* fn) {
		data.relocs.clear();
		if(d.isArray && (isArrayType(d.type) || isStruct(d.type)))
			return registerGlobalAggArray(d, sym, fn);
		if(d.isArray)
			return registerGlobalArrayOfScalar(d, sym, fn);
		if(isStruct(d.type))
			return registerGlobalStruct(d, sym, fn);
		return registerGlobalScalar(d, sym, fn);
	}

	B32 Emitter::registerGlobalStruct(const Declarator& d, const String& sym, Function* fn) {
		const StructType* st = d.type.strukt;
		const Expr* sinit = d.init ? peelAggregateCompound(d.init) : nullptr;
		U32 flex = flexElemCount(st, sinit);
		U32 total = st->size;
		if(flex > 0)
			total += flex * byteSize(st->fields.back().type);
		List<U8> init;

		data.flexCount = flex;
		B32 ok = true;
		if(sinit && sinit->kind == ExprKind::InitList) {
			init.assign(total, 0);
			ImageSink sink(*this, init);
			ok = initStructInit(sink, 0, st, sinit);
		} else if(sinit) {
			diag.fail("invalid initializer for struct '" + *d.name + "'");
			ok = false;
		}
		data.flexCount = 0;
		if(!ok)
			return false;

		defineGlobal(d, sym, byteArrayType(total), std::move(init));
		bindGlobal(d, sym, fn, false, 0);
		return true;
	}

	B32 Emitter::registerGlobalScalar(const Declarator& d, const String& sym, Function* fn) {
		if(d.type.isVoid() && !isPointer(d.type)) {
			diag.fail("variable '" + *d.name + "' has incomplete type 'void'");
			return false;
		}
		const Expr* dinit = d.init;
		if(dinit && dinit->kind == ExprKind::InitList && dinit->args.size() == 1 &&
			 dinit->args[0]->kind != ExprKind::InitList)
			dinit = dinit->args[0];
		U64 value = 0;
		List<U8> init;
		if(dinit && isFloating(d.type)) {
			F80 dv = 0;
			if(!evalFloatConst(dinit, dv)) {
				diag.fail("initializer for '" + *d.name + "' is not a constant expression");
				return false;
			}
			encodeFloatBytes(d.type, dv, init);
		} else if(dinit) {
			I64 iv = 0;
			if(!evalConst(dinit, iv)) {
				String target;
				I64 add = 0;
				B32 isIntScalar = !isPointer(d.type) && !isAggregate(d.type) && !isVoidType(d.type);
				B32 fits = isPointer(d.type) || (isIntScalar && byteSize(d.type) >= 8);
				if(!fits || !evalAddrConst(dinit, target, add)) {
					diag.fail("initializer for '" + *d.name + "' is not a constant expression");
					return false;
				}
				data.relocs.push_back(Reloc{0, target, add});
			}
			value = (U64)iv;
		}
		U32 bytes = byteSize(d.type);
		if(init.empty())
			for(U32 i = 0; i < bytes; ++i)
				init.push_back((U8)(value >> (8 * i)));
		init.resize(bytes, 0);
		defineGlobal(d, sym, irType(d.type), std::move(init));
		bindGlobal(d, sym, fn, false, 0);
		return true;
	}

	B32 Emitter::registerGlobalAlias(const Declarator& d) {
		if(d.init) {
			diag.fail("alias '" + *d.name + "' must not have an initializer");
			return false;
		}
		I64 count = 0;
		if(d.isArray && d.arrayLen && !evalConst(d.arrayLen, count))
			count = 0;
		mod.createAlias(*d.name, *d.aliasOf, irType(d.type))
				->setLinkage(d.isStatic ? Global::Linkage::Internal : Global::Linkage::External);
		syms.globals[*d.name] = GlobalVar{d.type, d.isArray, (U32)count};
		syms.aliases[*d.name] = *d.aliasOf;
		return true;
	}

	B32 Emitter::registerGlobals(const TransUnit& unit) {
		List<const Declarator*> order;
		List<B32> needsStorage;
		Map<String, U32> seen;
		for(const Stmt* decl : unit.globals) {
			for(const Declarator& d : decl->decls) {
				B32 defines = d.init != nullptr || !d.isExtern;
				auto it = seen.find(*d.name);
				if(it == seen.end()) {
					seen[*d.name] = (U32)order.size();
					order.push_back(&d);
					needsStorage.push_back(defines);
					continue;
				}
				const Declarator*& prev = order[it->second];
				if(d.init && prev->init) {
					diag.fail("redefinition of '" + *d.name + "'");
					return false;
				}
				if(d.init && !prev->init)
					prev = &d;
				if(defines)
					needsStorage[it->second] = true;
			}
		}
		for(U32 gi = 0; gi < order.size(); ++gi) {
			const Declarator& d = *order[gi];
			if(d.aliasOf) {
				if(!registerGlobalAlias(d))
					return false;
				continue;
			}
			if(!needsStorage[gi]) {
				I64 count = 0;
				if(d.isArray && d.arrayLen)
					evalConst(d.arrayLen, count);
				syms.globals[*d.name] = GlobalVar{d.type, d.isArray, (U32)count};
				continue;
			}
			if(!registerGlobal(d, *d.name, nullptr))
				return false;
		}
		return true;
	}
} // namespace rat::cc
