#include "emit/emit.h"

namespace rat::cc {
	String Emitter::internString(const Expr* e) {
		const String& bytes = *e->str.bytes;
		U32 cw = e->str.charSize;
		String key = std::to_string(cw) + ":" + bytes;
		auto it = data.stringPool.find(key);
		if(it != data.stringPool.end())
			return it->second;
		String name = data.nextName("__ratcc_str");
		List<U8> init(bytes.size() + cw, 0); // zero terminated
		std::copy(bytes.begin(), bytes.end(), init.begin());
		Global* g = mod.createGlobal(name, byteArrayType((U32)init.size()), true, std::move(init));
		g->setLinkage(Global::Linkage::Internal);
		data.stringPool.emplace(std::move(key), name);
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

	// length of an array compound literal, false when it is not a positive constant
	B32 Emitter::compoundArrayCount(const Expr* e, I64& count) {
		CType ty = e->compound.type;
		const Expr* init = e->compound.init;
		count = 0;
		if(!isUnsizedArray(ty))
			return arrayBound(ty, count) && count > 0;
		if(init->kind == ExprKind::StrLit)
			count = (I64)init->str.bytes->size() + 1;
		else if(init->kind == ExprKind::InitList)
			count = (I64)initArrayCount(arrayElem(ty), init);
		return count > 0;
	}

	B32 Emitter::internCompoundLiteral(const Expr* e, String& outSym) {
		CType ty = e->compound.type;
		const Expr* init = e->compound.init;
		List<Reloc> saved;
		saved.swap(data.relocs);
		B32 ok = true;
		String name = data.nextName("__ratcc_cl");

		B32 isArr = isArrayType(ty);
		CType elem = isArr ? arrayElem(ty) : ty;
		U32 total = byteSize(ty);
		I64 count = 0;
		if(isArr) {
			ok = compoundArrayCount(e, count);
			total = (U32)count * byteSize(elem);
		}

		if(ok) {
			List<U8> img(total, 0);
			ImageSink sink(*this, img);
			if(isArr && init->kind == ExprKind::StrLit)
				ok = sink.charArray(0, elem, (U32)count, init);
			else if(isArr)
				ok = initArrayInit(sink, 0, elem, (U32)count, init);
			else if(isStruct(ty))
				ok = initStructInit(sink, 0, ty.strukt, init);
			else
				ok = sink.scalar(0, ty, init);
			if(ok) {
				Type* imgTy = byteArrayType(total);
				Global* g = mod.createGlobal(name, imgTy, false, std::move(img), std::move(data.relocs));
				g->setLinkage(Global::Linkage::Internal);
			}
		}

		data.relocs.swap(saved);
		if(!ok)
			return diag.fail("invalid file-scope compound literal initializer");
		syms.globals[name] = GlobalVariable{ty};
		outSym = name;
		return true;
	}

	void Emitter::bindGlobal(const Declarator& d, const String& sym, Function* fn, CType type) {
		if(fn) {
			Local loc = Local::mem(fn->global(sym), type);
			loc.staticSymbol = arena.make<String>(sym);
			func.scopes.set(*d.name, loc);
		} else {
			syms.globals[*d.name] = GlobalVariable{type};
		}
	}

	void Emitter::defineGlobal(const Declarator& d, const String& sym, Type* ty, List<U8>&& img) {
		Global* g = mod.createGlobal(sym, ty, false, std::move(img), std::move(data.relocs));
		if(d.isStatic)
			g->setLinkage(Global::Linkage::Internal);
		g->setAlign(d.align);
	}

	B32 Emitter::registerStringArray(const Declarator& d, const String& sym, Function* fn, I64 len) {
		CType elem = arrayElem(d.type);
		U32 elemSize = byteSize(elem);
		U32 charWidth = d.init->str.isWide ? d.init->str.charSize : 1u;
		if(elem.ptr != 0 || elem.bits != charWidth * 8)
			return failStringNeedsCharArray();
		const String& bytes = *d.init->str.bytes;
		I64 nchars = (I64)bytes.size() / (I64)charWidth;
		if(len == 0)
			len = nchars + 1;
		List<U8> init((U32)len * elemSize, 0);
		for(I64 i = 0; i < nchars && i < len; ++i)
			for(U32 k = 0; k < charWidth; ++k)
				init[(U32)(i * elemSize) + k] = (U8)bytes[(U32)(i * charWidth + k)];
		defineGlobal(d, sym, mod.getArray(irType(elem), (U32)len), std::move(init));
		bindGlobal(d, sym, fn, sizedArray(arena, d.type, (U64)len));
		return true;
	}

	B32 Emitter::registerGlobalArray(const Declarator& d, const String& sym, Function* fn) {
		CType elem = arrayElem(d.type);
		B32 scalarElem = !isArrayType(elem) && !isStruct(elem);
		B32 haveLen = !isUnsizedArray(d.type);
		I64 count;
		if(!arrayBound(d.type, count) || (haveLen && count <= 0))
			return failArrayCount();
		if(scalarElem && d.init && d.init->kind == ExprKind::StrLit)
			return registerStringArray(d, sym, fn, count);
		B32 isList = d.init && d.init->kind == ExprKind::InitList;
		if(scalarElem && d.init && !isList)
			return diag.fail("invalid initializer for an array");
		if(!haveLen) {
			if(!isList)
				return failArrayUnknownSize(*d.name);
			count = (I64)initArrayCount(elem, d.init);
		}
		if(scalarElem && count <= 0)
			return failArrayCount();
		U32 total = (U32)count * byteSize(elem);
		List<U8> img;
		if(d.init) {
			img.assign(total, 0);
			ImageSink sink(*this, img);
			U32 pos = 0;
			if(isStruct(elem) && initListIsFlat(d.init)) {
				if(!initFlatArray(sink, 0, elem, (U32)count, {d.init->args, pos}))
					return false;
			} else if(!initArrayInit(sink, 0, elem, (U32)count, d.init))
				return false;
		}
		Type* type = scalarElem ? mod.getArray(irType(elem), (U32)count) : byteArrayType(total);
		defineGlobal(d, sym, type, std::move(img));
		bindGlobal(d, sym, fn, sizedArray(arena, d.type, (U64)count));
		return true;
	}

	B32 Emitter::registerGlobal(const Declarator& d, const String& sym, Function* fn) {
		data.relocs.clear();
		if(isArrayType(d.type))
			return registerGlobalArray(d, sym, fn);
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
			total += flex * byteSize(arrayElem(st->fields.back().type));
		List<U8> init;
		if(sinit && sinit->kind != ExprKind::InitList)
			return diag.fail("invalid initializer for struct '" + *d.name + "'");
		if(sinit) {
			init.assign(total, 0);
			ImageSink sink(*this, init);
			data.flexCount = flex;
			B32 ok = initStructInit(sink, 0, st, sinit);
			data.flexCount = 0;
			if(!ok)
				return false;
		}
		defineGlobal(d, sym, byteArrayType(total), std::move(init));
		bindGlobal(d, sym, fn, d.type);
		return true;
	}

	B32 Emitter::registerGlobalScalar(const Declarator& d, const String& sym, Function* fn) {
		if(d.type.isVoid() && !isPointer(d.type))
			return diag.fail("variable '" + *d.name + "' has incomplete type 'void'");
		const Expr* dinit = d.init;
		B32 singleBraced = dinit && dinit->kind == ExprKind::InitList && dinit->args.size() == 1;
		if(singleBraced && dinit->args[0]->kind != ExprKind::InitList)
			dinit = dinit->args[0];
		List<U8> init(byteSize(d.type), 0);
		B32 isIntScalar = !isPointer(d.type) && !isAggregate(d.type) && !isVoidType(d.type);
		B32 addrFits = isPointer(d.type) || (isIntScalar && byteSize(d.type) >= 8);
		if(dinit && !writeConstScalar(init, 0, d.type, dinit, addrFits))
			return diag.fail("initializer for '" + *d.name + "' is not a constant expression");
		defineGlobal(d, sym, irType(d.type), std::move(init));
		bindGlobal(d, sym, fn, d.type);
		return true;
	}

	B32 Emitter::registerGlobalAlias(const Declarator& d) {
		if(d.init)
			return diag.fail("alias '" + *d.name + "' must not have an initializer");
		Linkage linkage = d.isStatic ? Linkage::Internal : Linkage::External;
		mod.createAlias(*d.name, *d.aliasOf, irType(d.type))->setLinkage(linkage);
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
				auto [it, fresh] = seen.try_emplace(*d.name, (U32)order.size());
				if(fresh) {
					order.push_back(&d);
					needsStorage.push_back(defines);
					continue;
				}
				const Declarator*& prev = order[it->second];
				if(d.init && prev->init)
					return diag.fail("redefinition of '" + *d.name + "'");
				if(d.init && !prev->init)
					prev = &d;
				if(defines)
					needsStorage[it->second] = true;
			}
		}
		for(U32 gi = 0; gi < order.size(); ++gi) {
			const Declarator& d = *order[gi];
			if(d.aliasOf && !registerGlobalAlias(d))
				return false;
			if(d.aliasOf || !needsStorage[gi]) {
				CType type = d.type;
				I64 count;
				if(isArrayType(type) && arrayBound(type, count))
					type = sizedArray(arena, type, (U64)count);
				syms.globals[*d.name] = GlobalVariable{type};
				continue;
			}
			if(!registerGlobal(d, *d.name, nullptr))
				return false;
			debugGlobal(d);
		}
		return true;
	}
} // namespace rat::cc
