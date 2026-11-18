#include "emit/emit.h"

namespace rat::cc {
	void Emitter::zeroSlot(Function& fn, Node* slot, U32 size) {
		for(U32 i = 0; i < size;) {
			U32 w = detail::alignedChunkWidth(i, size);
			Type* ty = mod.getInt(w * 8);
			fn.store(offsetPtr(fn, slot, i), fn.constInt(ty, 0));
			i += w;
		}
	}

	B32 Emitter::declareStatic(Function& fn, const Declarator& d) {
		String sym = "__ratcc_static" + std::to_string(data.staticCounter++) + "_" + *d.name;
		return registerGlobal(d, sym, &fn);
	}

	B32 Emitter::emitVlaDecl(Function& fn, const Declarator& d) {
		CType elem = arrayElem(d.type);
		if(!bindVlaDims(&fn, elem))
			return false;
		Node* elemBytes = emitArrayByteSize(fn, elem);
		if(!elemBytes)
			return false;
		Node* bytes = elemBytes;
		if(d.type.array->countExpr) {
			Node* len = vlaDimCount(fn, d.type);
			if(!len)
				return false;
			bytes = fn.mul(len, elemBytes);
		}
		Node* slot = fn.stackAlloc(bytes);
		func.sp = slot; // the block's exit restore hands this storage back
		Local loc = Local::mem(slot, sizedArray(arena, d.type, 0)); // debug info gets no outer bound
		loc.lengthNode = bytes;
		func.scopes.set(*d.name, loc);
		return true;
	}

	B32 Emitter::declareExtern(Function& fn, const Declarator& d) {
		CType type = d.type;
		auto g = syms.globals.find(*d.name);
		if(g != syms.globals.end())
			type = g->second.type;
		func.scopes.set(*d.name, Local::mem(fn.global(*d.name), type));
		return true;
	}

	B32 Emitter::declareDead(Function& fn, const Stmt* s) {
		B32 live = false;
		for(const Declarator& d : s->decls)
			if(!emitOneDecl(fn, d, live))
				return false;
		return true;
	}

	B32 Emitter::emitComplexDecl(Function& fn, const Declarator& d, B32 live) {
		CType ct = completeComplex(d.type);
		Node* slot = declSlot(fn, d, byteArrayType(ct.strukt->size), ct.strukt->size);
		if(live && d.init) {
			if(d.init->kind == ExprKind::InitList) {
				CType re = complexElem(ct);
				zeroSlot(fn, slot, ct.strukt->size);
				const List<Expr*>& els = d.init->args;
				for(U32 i = 0; i < els.size() && i < 2; ++i) { // real, then imaginary
					Value v = emitExpr(fn, els[i]);
					if(!v.node)
						return false;
					fn.store(offsetPtr(fn, slot, i * byteSize(re)), convert(fn, v.node, v.type, re));
				}
			} else {
				Value v = emitExpr(fn, d.init);
				if(!v.node)
					return false;
				storeComplex(fn, slot, ct, v);
			}
		}
		func.scopes.set(*d.name, Local::mem(slot, ct));
		return true;
	}

	B32 Emitter::emitStructDecl(Function& fn, const Declarator& d, B32 live) {
		Node* slot = declSlot(fn, d, byteArrayType(d.type.strukt->size), d.type.strukt->size);
		if(live && d.init && d.init->kind == ExprKind::InitList) {
			zeroSlot(fn, slot, d.type.strukt->size);
			StoreSink sink(*this, fn, slot);
			if(!initStructInit(sink, 0, d.type.strukt, d.init))
				return false;
		} else if(live && d.init) {
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			if(!isStruct(v.type) || v.type.strukt != d.type.strukt)
				return diag.fail("invalid initializer for struct '" + *d.name + "'");
			emitMemCopy(fn, slot, v.node, d.type.strukt->size);
		}
		func.scopes.set(*d.name, Local::mem(slot, d.type));
		return true;
	}

	B32 Emitter::emitStringArrayDecl(Function& fn, const Declarator& d, I64 count, B32 live) {
		CType elem = arrayElem(d.type);
		Type* elemTy = irType(elem);
		U32 elemSize = byteSize(elem);
		U32 cw = d.init->str.isWide ? d.init->str.charSize : 1u;
		if(!checkStringElem(elem, cw))
			return false;
		const String& bytes = *d.init->str.bytes;
		I64 nchars = (I64)bytes.size() / (I64)cw;
		if(isUnsizedArray(d.type))
			count = nchars + 1;
		Node* slot = declSlot(fn, d, mod.getArray(elemTy, (U32)count), (U32)count * elemSize);
		if(live) {
			for(I64 i = 0; i < count; ++i) {
				I64 val = 0;
				if(i < nchars)
					for(U32 k = 0; k < cw; ++k)
						val |= (I64)(U8)bytes[(U32)(i * cw + k)] << (8 * k);
				fn.store(offsetPtr(fn, slot, (U64)i * elemSize), fn.constInt(elemTy, val));
			}
		}
		func.scopes.set(*d.name, Local::mem(slot, sizedArray(arena, d.type, (U64)count)));
		return true;
	}

	B32 Emitter::emitArrayDecl(Function& fn, const Declarator& d, B32 live) {
		CType elem = arrayElem(d.type);
		B32 scalarElem = !isArrayType(elem) && !isStruct(elem);
		B32 haveLen = !isUnsizedArray(d.type);
		I64 count;
		if(!arrayBound(d.type, count) || hasVlaDim(elem)) {
			if(live && d.init)
				return diag.fail("a variable-length array may not be initialized");
			return emitVlaDecl(fn, d);
		}
		if(haveLen && count <= 0)
			return failArrayCount();
		if(scalarElem && d.init && d.init->kind == ExprKind::StrLit)
			return emitStringArrayDecl(fn, d, count, live);
		B32 isList = d.init && d.init->kind == ExprKind::InitList;
		if(scalarElem && live && d.init && !isList)
			return diag.fail("invalid initializer for an array");
		if(!haveLen) {
			if(!isList)
				return failArrayUnknownSize(*d.name);
			count = (I64)initArrayCount(elem, d.init);
		}
		if(!isArrayType(elem) && count <= 0)
			return failArrayCount();
		U32 total = (U32)count * byteSize(elem);
		Type* type = scalarElem ? mod.getArray(irType(elem), (U32)count) : byteArrayType(total);
		Node* slot = declSlot(fn, d, type, total);
		if(live && (d.init || !scalarElem)) {
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(d.init && !initArrayInit(sink, 0, elem, (U32)count, d.init))
				return false;
		}
		func.scopes.set(*d.name, Local::mem(slot, sizedArray(arena, d.type, (U64)count)));
		return true;
	}

	B32 Emitter::emitOneDecl(Function& fn, const Declarator& d, B32 live) {
		if(d.type.func && d.type.ptr == 0) {
			Local proto;
			proto.kind = Local::Kind::Function;
			func.scopes.set(*d.name, proto);
			return true;
		}
		if(d.isTypedef)
			return bindVlaDims(&fn, d.type);
		if(d.isExtern)
			return declareExtern(fn, d);
		if(d.isStatic)
			return declareStatic(fn, d);
		if(isArrayType(d.type))
			return emitArrayDecl(fn, d, live);
		if(isComplexType(d.type))
			return emitComplexDecl(fn, d, live);
		if(isStruct(d.type))
			return emitStructDecl(fn, d, live);
		if(!live) {
			Node* slot = declSlot(fn, d, irType(d.type), byteSize(d.type));
			func.scopes.set(*d.name, Local::mem(slot, d.type));
			return true;
		}
		if(d.init && exprRefersTo(d.init, *d.name)) {
			Node* slot = declSlot(fn, d, irType(d.type), byteSize(d.type));
			func.scopes.set(*d.name, Local::mem(slot, d.type));
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			Node* val = convert(fn, v.node, v.type, d.type);
			fn.store(slot, val, isTopVolatile(d.type));
			return true;
		}
		Node* init;
		if(d.init) {
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			init = convert(fn, v.node, v.type, d.type);
		} else {
			init = fn.constInt(irType(d.type), 0);
		}
		// a volatile local lives in memory, so every access stays
		if(func.addrTaken.count(*d.name) || isTopVolatile(d.type) || dbg.keepLocals) {
			Node* slot = declSlot(fn, d, irType(d.type), byteSize(d.type));
			fn.store(slot, init, isTopVolatile(d.type));
			func.scopes.set(*d.name, Local::mem(slot, d.type));
		} else {
			func.scopes.set(*d.name, Local::inVar(fn.declareLocal(*d.name, init), d.type));
		}
		return true;
	}
} // namespace rat::cc
