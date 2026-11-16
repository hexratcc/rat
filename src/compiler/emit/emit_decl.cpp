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

	B32 Emitter::declIsVla(const Declarator& d, I64& count) {
		count = 0;
		B32 outerVla = d.arrayLen && !evalConst(d.arrayLen, count);
		return outerVla || isVlaType(d.type);
	}

	B32 Emitter::emitVlaDecl(Function& fn, const Declarator& d) {
		Node* elemBytes = emitArrayByteSize(fn, d.type);
		if(!elemBytes)
			return false;
		Node* bytes = elemBytes;
		if(d.arrayLen) {
			Value v = emitExpr(fn, d.arrayLen);
			if(!v.node)
				return false;
			Node* len = convert(fn, v.node, v.type, ctSize());
			bytes = fn.mul(len, elemBytes);
		}
		Node* slot = fn.stackAlloc(bytes);
		func.sp = slot; // the block's exit restore hands this storage back
		Local loc = Local::memArray(slot, d.type);
		loc.lengthNode = bytes;
		func.scopes.set(*d.name, loc);
		return true;
	}

	B32 Emitter::declareExtern(Function& fn, const Declarator& d) {
		Node* addr = fn.global(*d.name);
		GlobalVariable gv{d.type, d.isArray, 0};
		auto g = syms.globals.find(*d.name);
		if(g != syms.globals.end())
			gv = g->second;
		if(gv.isArray)
			func.scopes.set(*d.name, Local::memArray(addr, gv.type, gv.count));
		else if(isArrayType(gv.type))
			func.scopes.set(*d.name, Local::memArray(addr, arrayElem(gv.type)));
		else
			func.scopes.set(*d.name, Local::mem(addr, gv.type));
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

	B32 Emitter::emitTypedefArrayDecl(Function& fn, const Declarator& d, B32 live) {
		if(live && d.init)
			return diag.fail("invalid initializer for array variable '" + *d.name + "'");
		Node* slot = declSlot(fn, d, byteArrayType(byteSize(d.type)), byteSize(d.type));
		func.scopes.set(*d.name, Local::memArray(slot, arrayElem(d.type)));
		return true;
	}

	B32 Emitter::emitAggArrayDecl(
			Function& fn, const Declarator& d, I64 count, B32 haveLen, B32 live) {
		if(!haveLen) {
			if(!d.init || d.init->kind != ExprKind::InitList)
				return failArrayUnknownSize(*d.name);
			count = (I64)initArrayCount(d.type, d.init);
		}
		if(isStruct(d.type) && count <= 0)
			return failArrayCount();
		U32 elemSize = byteSize(d.type);
		U32 total = (U32)count * elemSize;
		Node* slot = declSlot(fn, d, byteArrayType(total), total);
		if(live) {
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
				return false;
		}
		func.scopes.set(*d.name, Local::memArray(slot, d.type, (U32)count));
		return true;
	}

	B32 Emitter::emitArrayDecl(Function& fn, const Declarator& d, B32 live) {
		if(!d.isArray && isArrayType(d.type))
			return emitTypedefArrayDecl(fn, d, live);
		B32 haveLen = d.arrayLen != nullptr;
		I64 count;
		if(declIsVla(d, count)) {
			if(live && d.init)
				return diag.fail("a variable-length array may not be initialized");
			return emitVlaDecl(fn, d);
		}
		if(haveLen && count <= 0)
			return failArrayCount();
		if(isArrayType(d.type) || isStruct(d.type))
			return emitAggArrayDecl(fn, d, count, haveLen, live);
		Type* elemTy = irType(d.type);
		U32 elemSize = byteSize(d.type);

		if(d.init && d.init->kind == ExprKind::StrLit) {
			U32 cw = d.init->str.isWide ? d.init->str.charSize : 1u;
			if(d.type.ptr != 0 || d.type.bits != cw * 8)
				return failStringNeedsCharArray();
			const String& bytes = *d.init->str.bytes;
			I64 nchars = (I64)bytes.size() / (I64)cw;
			if(!haveLen)
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
			func.scopes.set(*d.name, Local::memArray(slot, d.type, (U32)count));
			return true;
		}

		if(d.init && d.init->kind == ExprKind::InitList) {
			const List<Expr*>& els = d.init->args;
			List<I64> idx;
			if(!resolveArrayIndices(d.init, haveLen, count, idx))
				return false;
			Node* slot = declSlot(fn, d, mod.getArray(elemTy, (U32)count), (U32)count * elemSize);
			if(live) {
				zeroSlot(fn, slot, (U32)count * elemSize);
				for(U32 i = 0; i < els.size(); ++i) {
					Value v = emitExpr(fn, els[i]);
					if(!v.node)
						return false;
					Node* val = convert(fn, v.node, v.type, d.type);
					fn.store(offsetPtr(fn, slot, (U64)idx[i] * elemSize), val);
				}
			}
			func.scopes.set(*d.name, Local::memArray(slot, d.type, (U32)count));
			return true;
		}

		if(live && d.init)
			return diag.fail("invalid initializer for an array");
		if(!haveLen)
			return failArrayUnknownSize(*d.name);
		Node* slot = declSlot(fn, d, mod.getArray(elemTy, (U32)count), (U32)count * elemSize);
		func.scopes.set(*d.name, Local::memArray(slot, d.type, (U32)count));
		return true;
	}

	B32 Emitter::emitOneDecl(Function& fn, const Declarator& d, B32 live) {
		if(d.isExtern)
			return declareExtern(fn, d);
		if(d.isStatic)
			return declareStatic(fn, d);
		if(!d.isArray && isComplexType(d.type))
			return emitComplexDecl(fn, d, live);
		if(!d.isArray && isStruct(d.type))
			return emitStructDecl(fn, d, live);
		if(isArrayType(d.type) || d.isArray)
			return emitArrayDecl(fn, d, live);
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
