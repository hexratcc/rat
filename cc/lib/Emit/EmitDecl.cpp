#include "Emit/Emit.h"

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
		String sym = "__ratcc_static" + std::to_string(staticCounter++) + "_" + *d.name;
		return d.isArray					? registerGlobalArray(d, sym, &fn)
					 : isStruct(d.type) ? registerGlobalStruct(d, sym, &fn)
															: registerGlobalScalar(d, sym, &fn);
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
		Node* slot = fn.allocVLA(irType(d.type), bytes);
		Local loc = Local::memArray(slot, d.type);
		loc.lengthNode = bytes;
		declare(*d.name, loc);
		return true;
	}

	B32 Emitter::declareDead(Function& fn, const Stmt* s) {
		for(const Declarator& d : s->decls) {
			if(d.isStatic) {
				if(!declareStatic(fn, d))
					return false;
				continue;
			}
			if(d.isArray) {
				I64 count;
				if(declIsVla(d, count)) {
					if(!emitVlaDecl(fn, d))
						return false;
					continue;
				}
				if(d.arrayLen) {
					if(count <= 0) {
						failArrayCount();
						return false;
					}
				} else if(d.init && d.init->kind == ExprKind::InitList) {
					count = (I64)d.init->args.size();
				} else {
					failArrayUnknownSize(*d.name);
					return false;
				}
				U32 total = (U32)count * byteSize(d.type);
				Node* slot = allocBytes(fn, total);
				declare(*d.name, Local::memArray(slot, d.type, (U32)count));
				continue;
			}
			if(isAggregate(d.type)) {
				Node* slot = allocBytes(fn, d.type.strukt->size);
				declare(*d.name, Local::mem(slot, d.type));
				continue;
			}
			if(isArrayType(d.type)) {
				Node* slot = allocBytes(fn, byteSize(d.type));
				declare(*d.name, Local::memArray(slot, arrayElem(d.type)));
				continue;
			}
			Node* slot = fn.alloc(irType(d.type));
			declare(*d.name, Local::mem(slot, d.type));
		}
		return true;
	}

	B32 Emitter::emitDecl(Function& fn, const Stmt* s) {
		for(const Declarator& d : s->decls)
			if(!emitOneDecl(fn, d))
				return false;
		return true;
	}

	B32 Emitter::emitComplexDecl(Function& fn, const Declarator& d) {
		CType ct = completeComplex(d.type);
		Node* slot = allocBytes(fn, ct.strukt->size);
		if(d.init) {
			if(d.init->kind == ExprKind::InitList) {
				CType re = complexElem(ct);
				zeroSlot(fn, slot, ct.strukt->size);
				const List<Expr*>& els = d.init->args;
				if(!els.empty()) {
					Value rv = emitExpr(fn, els[0]);
					if(!rv.node)
						return false;
					fn.store(slot, convert(fn, rv.node, rv.type, re));
				}
				if(els.size() > 1) {
					Value iv = emitExpr(fn, els[1]);
					if(!iv.node)
						return false;
					fn.store(offsetPtr(fn, slot, byteSize(re)), convert(fn, iv.node, iv.type, re));
				}
			} else {
				Value v = emitExpr(fn, d.init);
				if(!v.node)
					return false;
				storeComplex(fn, slot, ct, v);
			}
		}
		declare(*d.name, Local::mem(slot, ct));
		return true;
	}

	B32 Emitter::emitStructDecl(Function& fn, const Declarator& d) {
		Node* slot = allocBytes(fn, d.type.strukt->size);
		if(d.init && d.init->kind == ExprKind::InitList) {
			zeroSlot(fn, slot, d.type.strukt->size);
			StoreSink sink(*this, fn, slot);
			if(!initStructInit(sink, 0, d.type.strukt, d.init))
				return false;
			declare(*d.name, Local::mem(slot, d.type));
			return true;
		}
		if(d.init) {
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			if(!isStruct(v.type) || v.type.strukt != d.type.strukt) {
				fail("invalid initializer for struct '" + *d.name + "'");
				return false;
			}
			emitMemCopy(fn, slot, v.node, d.type.strukt->size);
		}
		declare(*d.name, Local::mem(slot, d.type));
		return true;
	}

	B32 Emitter::emitTypedefArrayDecl(Function& fn, const Declarator& d) {
		if(d.init) {
			fail("invalid initializer for array variable '" + *d.name + "'");
			return false;
		}
		Node* slot = allocBytes(fn, byteSize(d.type));
		declare(*d.name, Local::memArray(slot, arrayElem(d.type)));
		return true;
	}

	B32 Emitter::emitMultiDimArrayDecl(Function& fn, const Declarator& d) {
		I64 count;
		if(declIsVla(d, count)) {
			if(d.init) {
				fail("a variable-length array may not be initialized");
				return false;
			}
			return emitVlaDecl(fn, d);
		}
		B32 haveLen = d.arrayLen != nullptr;
		if(haveLen && count <= 0) {
			failArrayCount();
			return false;
		}
		if(!haveLen) {
			if(!d.init || d.init->kind != ExprKind::InitList) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			count = (I64)arrayInitOuterExtent(d.type, d.init);
		}
		U32 elemSize = byteSize(d.type);
		U32 total = (U32)count * elemSize;
		Node* slot = allocBytes(fn, total);
		zeroSlot(fn, slot, total);
		StoreSink sink(*this, fn, slot);
		if(d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
			return false;
		declare(*d.name, Local::memArray(slot, d.type, (U32)count));
		return true;
	}

	B32 Emitter::emitArrayDecl(Function& fn, const Declarator& d) {
		if(!d.isArray && isArrayType(d.type))
			return emitTypedefArrayDecl(fn, d);
		if(d.isArray && isArrayType(d.type))
			return emitMultiDimArrayDecl(fn, d);
		B32 haveLen = d.arrayLen != nullptr;
		I64 count;
		if(declIsVla(d, count)) {
			if(d.init) {
				fail("a variable-length array may not be initialized");
				return false;
			}
			return emitVlaDecl(fn, d);
		}
		if(haveLen && count <= 0) {
			failArrayCount();
			return false;
		}
		Type* elemTy = irType(d.type);
		U32 elemSize = byteSize(d.type);

		if(isStruct(d.type)) {
			if(!haveLen) {
				if(!d.init || d.init->kind != ExprKind::InitList) {
					failArrayUnknownSize(*d.name);
					return false;
				}
				count = (I64)arrayInitOuterExtent(d.type, d.init);
			}
			if(count <= 0) {
				failArrayCount();
				return false;
			}
			U32 total = (U32)count * elemSize;
			Node* slot = allocBytes(fn, total);
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
				return false;
			declare(*d.name, Local::memArray(slot, d.type, (U32)count));
			return true;
		}

		if(d.init && d.init->kind == ExprKind::StrLit) {
			U32 cw = d.init->str.isWide ? d.init->str.charSize : 1u;
			if(d.type.ptr != 0 || isStruct(d.type) || d.type.bits != cw * 8) {
				failStringNeedsCharArray();
				return false;
			}
			const String& bytes = *d.init->str.bytes;
			I64 nchars = (I64)bytes.size() / (I64)cw;
			if(!haveLen)
				count = nchars + 1;
			Node* slot = fn.alloc(mod.getArray(elemTy, (U32)count));
			for(I64 i = 0; i < count; ++i) {
				I64 val = 0;
				if(i < nchars)
					for(U32 k = 0; k < cw; ++k)
						val |= (I64)(U8)bytes[(U32)(i * cw + k)] << (8 * k);
				fn.store(offsetPtr(fn, slot, (U64)i * elemSize), fn.constInt(elemTy, val));
			}
			declare(*d.name, Local::memArray(slot, d.type, (U32)count));
			return true;
		}

		if(d.init && d.init->kind == ExprKind::InitList) {
			const List<Expr*>& els = d.init->args;
			const List<Designator>& des = d.init->designators;
			List<I64> idx(els.size());
			I64 maxIdx = -1;
			if(!resolveArrayIndices(els, des, idx, maxIdx))
				return false;
			if(!haveLen)
				count = maxIdx + 1;
			else if(maxIdx >= count) {
				failTooManyInits();
				return false;
			}
			if(count <= 0) {
				failArrayCount();
				return false;
			}
			Node* slot = fn.alloc(mod.getArray(elemTy, (U32)count));
			zeroSlot(fn, slot, (U32)count * elemSize);
			for(U32 i = 0; i < els.size(); ++i) {
				Value v = emitExpr(fn, els[i]);
				if(!v.node)
					return false;
				Node* val = convert(fn, v.node, v.type, d.type);
				fn.store(offsetPtr(fn, slot, (U64)idx[i] * elemSize), val);
			}
			declare(*d.name, Local::memArray(slot, d.type, (U32)count));
			return true;
		}

		if(d.init) {
			fail("invalid initializer for an array");
			return false;
		}
		if(!haveLen) {
			failArrayUnknownSize(*d.name);
			return false;
		}
		Node* slot = fn.alloc(mod.getArray(irType(d.type), (U32)count));
		declare(*d.name, Local::memArray(slot, d.type, (U32)count));
		return true;
	}

	B32 Emitter::emitOneDecl(Function& fn, const Declarator& d) {
		if(d.isStatic)
			return declareStatic(fn, d);
		if(!d.isArray && isComplexType(d.type))
			return emitComplexDecl(fn, d);
		if(!d.isArray && isStruct(d.type))
			return emitStructDecl(fn, d);
		if(isArrayType(d.type) || d.isArray)
			return emitArrayDecl(fn, d);
		if(d.init && exprRefersTo(d.init, *d.name)) {
			Node* slot = fn.alloc(irType(d.type));
			declare(*d.name, Local::mem(slot, d.type));
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			Node* val = convert(fn, v.node, v.type, d.type);
			fn.store(slot, val);
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
		if(memVars.count(*d.name)) {
			Node* slot = fn.alloc(irType(d.type));
			fn.store(slot, init);
			declare(*d.name, Local::mem(slot, d.type));
		} else {
			declare(*d.name, Local::inVar(fn.declareLocal(*d.name, init), d.type));
		}
		return true;
	}
} // namespace rat::cc
