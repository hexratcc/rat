#include "Emit.h"

namespace rat::cc {
	B32 Emitter::initFlatObject(InitSink& sink,
															U32 base,
															CType ty,
															const List<Expr*>& els,
															U32& pos,
															const List<Designator>* des) {
		if(pos >= els.size())
			return true;
		const Expr* e = els[pos];
		if(isStruct(ty)) {
			const Expr* peeled = peelAggregateCompound(e);
			if(peeled != e || e->kind == ExprKind::InitList) {
				++pos;
				return initStructInit(sink, base, ty.strukt, peeled);
			}
			CType et;
			if(typeOf(e, et) && isStruct(et) && et.strukt == ty.strukt) {
				++pos;
				return sink.structCopy(base, ty, e);
			}
			return initFlatStruct(sink, base, ty.strukt, els, pos, des);
		}
		if(isArrayType(ty)) {
			CType el = arrayElem(ty);
			U32 cnt = ty.array->count;
			if(e->kind == ExprKind::InitList) {
				++pos;
				return initArrayInit(sink, base, el, cnt, e);
			}
			if(isCharType(el) && e->kind == ExprKind::StrLit) {
				++pos;
				return sink.charArray(base, el, cnt, e);
			}
			return initFlatArray(sink, base, el, cnt, els, pos, des);
		}
		++pos;
		return sink.scalar(base, ty, e);
	}

	B32 Emitter::initFlatStruct(InitSink& sink,
															U32 base,
															const StructType* st,
															const List<Expr*>& els,
															U32& pos,
															const List<Designator>* des) {
		B32 first = true;
		for(U32 fi = 0; fi < st->fields.size() && pos < els.size(); ++fi) {
			if(!first && des && (*des)[pos].isSet)
				break;
			first = false;
			const Field& f = st->fields[fi];
			if(f.anonMember && !f.anonFirst)
				continue;
			if(f.isArray) {
				const Expr* e = els[pos];
				if(e->kind == ExprKind::InitList) {
					++pos;
					if(!initArrayInit(sink, base + f.offset, f.type, f.count, e))
						return false;
				} else if(isCharType(f.type) && e->kind == ExprKind::StrLit) {
					++pos;
					if(!sink.charArray(base + f.offset, f.type, f.count, e))
						return false;
				} else if(!initFlatArray(sink, base + f.offset, f.type, f.count, els, pos, des))
					return false;
				if(st->isUnion)
					break;
				continue;
			}
			if(f.isBitfield) {
				if(!sink.bitfield(base + f.offset, f.type, f.bitWidth, f.bitOffset, els[pos]))
					return false;
				++pos;
				if(st->isUnion)
					break;
				continue;
			}
			if(!initFlatObject(sink, base + f.offset, f.type, els, pos, des))
				return false;
			if(st->isUnion)
				break;
		}
		return true;
	}

	B32 Emitter::initFlatArray(InitSink& sink,
														 U32 base,
														 CType elem,
														 U32 count,
														 const List<Expr*>& els,
														 U32& pos,
														 const List<Designator>* des) {
		U32 esz = byteSize(elem);
		for(U32 i = 0; i < count && pos < els.size(); ++i) {
			if(i != 0 && des && (*des)[pos].isSet)
				break;
			if(!initFlatObject(sink, base + i * esz, elem, els, pos, des))
				return false;
		}
		return true;
	}

	B32 Emitter::StoreSink::scalar(U32 off, CType dt, const Expr* e) {
		return emit.storeScalar(fn, slot, off, dt, e);
	}
	B32 Emitter::StoreSink::bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) {
		B32 skip = false;
		if(!emit.unwrapScalarInit(e, skip))
			return false;
		if(skip)
			return true;
		Value v = emit.emitExpr(fn, e);
		if(!v.node)
			return false;
		Node* val = emit.convert(fn, v.node, v.type, dt);
		LValue lv;
		lv.addr = emit.offsetPtr(fn, slot, off);
		lv.type = dt;
		lv.isBitfield = true;
		lv.bitWidth = width;
		lv.bitOffset = bitOff;
		emit.storeLValue(fn, lv, val);
		return true;
	}
	B32 Emitter::StoreSink::charArray(U32 base, CType elem, U32 count, const Expr* e) {
		return emit.storeCharArray(fn, slot, base, elem, count, e);
	}
	B32 Emitter::StoreSink::structCopy(U32 off, CType ty, const Expr* e) {
		Value v = emit.emitExpr(fn, e);
		if(!v.node)
			return false;
		Node* dst = off ? fn.add(slot, fn.constInt(emit.irType(emit.ctSize()), off)) : slot;
		emit.emitMemCopy(fn, dst, v.node, ty.strukt->size);
		return true;
	}

	void Emitter::encodeFloatBytes(CType dt, long double v, List<U8>& out) {
		if(dt.bits == 32) {
			float f = (float)v;
			const U8* p = (const U8*)&f;
			for(U32 i = 0; i < sizeof(f); ++i)
				out.push_back(p[i]);
		} else if(dt.bits == 128) {
			// x86 extended precision
			long double ld = v;
			const U8* p = (const U8*)&ld;
			U32 n = byteSize(dt); // 16
			for(U32 i = 0; i < n; ++i)
				out.push_back(i < 10 ? p[i] : (U8)0);
		} else {
			double d = (double)v;
			const U8* p = (const U8*)&d;
			for(U32 i = 0; i < sizeof(d); ++i)
				out.push_back(p[i]);
		}
	}

	B32 Emitter::ImageSink::scalar(U32 off, CType dt, const Expr* e) {
		B32 skip = false;
		if(!emit.unwrapScalarInit(e, skip))
			return false;
		if(skip)
			return true;
		U64 bits = 0;
		if(isFloating(dt)) {
			long double d = 0;
			if(!emit.evalFloatConst(e, d)) {
				emit.failNonConstInit();
				return false;
			}
			List<U8> fb;
			emit.encodeFloatBytes(dt, d, fb);
			for(U32 b = 0; b < fb.size() && off + b < img.size(); ++b)
				img[off + b] = fb[b];
			return true;
		} else {
			I64 v = 0;
			if(!emit.evalConst(e, v)) {
				if(isPointer(dt)) {
					String sym;
					I64 add = 0;
					if(emit.evalAddrConst(e, sym, add)) {
						for(U32 r = 0; r < emit.relocs.size(); ++r)
							if(emit.relocs[r].offset == off) {
								emit.relocs[r] = Reloc{off, sym, add};
								return true;
							}
						emit.relocs.push_back(Reloc{off, sym, add});
						return true;
					}
				}
				emit.failNonConstInit();
				return false;
			}
			bits = (U64)v;
		}
		U32 sz = emit.byteSize(dt);
		for(U32 b = 0; b < sz; ++b)
			img[off + b] = (U8)(bits >> (8 * b));
		return true;
	}

	B32 Emitter::ImageSink::bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) {
		I64 v = 0;
		if(!emit.evalConst(e, v)) {
			emit.failNonConstInit();
			return false;
		}
		U32 sz = emit.byteSize(dt);
		U64 maskBits = width >= 64 ? ~0ull : ((1ull << width) - 1);
		U64 placed = ((U64)v & maskBits) << bitOff;
		U64 unit = 0;
		for(U32 b = 0; b < sz; ++b)
			unit |= (U64)img[off + b] << (8 * b);
		unit |= placed;
		for(U32 b = 0; b < sz; ++b)
			img[off + b] = (U8)(unit >> (8 * b));
		return true;
	}

	B32 Emitter::ImageSink::charArray(U32 base, CType elem, U32 count, const Expr* e) {
		if(!isCharType(elem)) {
			emit.failStringNeedsCharArray();
			return false;
		}
		const String& bytes = *e->str.bytes;
		U32 n = bytes.size() < count ? (U32)bytes.size() : count;
		for(U32 i = 0; i < n; ++i)
			img[base + i] = (U8)bytes[i];
		return true;
	}

	B32 Emitter::ImageSink::structCopy(U32, CType, const Expr*) {
		emit.failNonConstInit();
		return false;
	}

	const StructType* Emitter::anonGroupType(const StructType* st, U32 firstIdx) {
		StructType* g = arena.make<StructType>();
		g->isUnion = st->fields[firstIdx].anonUnion;
		U32 baseOff = st->fields[firstIdx].offset;
		for(U32 i = firstIdx; i < st->fields.size(); ++i) {
			const Field& f = st->fields[i];
			if(i != firstIdx && !(f.anonMember && !f.anonFirst))
				break;
			Field nf = f;
			nf.offset = f.offset - baseOff;
			nf.anonMember = false;
			nf.anonFirst = false;
			g->fields.push_back(nf);
		}
		g->size = 0;
		g->align = 1;
		return g;
	}

	U32 Emitter::scalarLeaves(CType ty) {
		if(isStruct(ty)) {
			U32 n = 0;
			for(U32 i = 0; i < ty.strukt->fields.size(); ++i) {
				const Field& f = ty.strukt->fields[i];
				if(f.anonMember && !f.anonFirst)
					continue;
				if(f.isArray)
					n += f.count * scalarLeaves(f.type);
				else
					n += scalarLeaves(f.type);
				if(ty.strukt->isUnion)
					break;
			}
			return n;
		}
		if(isArrayType(ty))
			return ty.array->count * scalarLeaves(arrayElem(ty));
		return 1;
	}

	void Emitter::flatConsumeObject(CType ty, const List<Expr*>& els, U32& pos) {
		if(pos >= els.size())
			return;
		const Expr* e = els[pos];
		if(isStruct(ty)) {
			if(e->kind == ExprKind::InitList || peelAggregateCompound(e) != e) {
				++pos;
				return;
			}
			for(U32 fi = 0; fi < ty.strukt->fields.size() && pos < els.size(); ++fi) {
				const Field& f = ty.strukt->fields[fi];
				if(f.anonMember && !f.anonFirst)
					continue;
				CType fty = f.type;
				if(f.isArray) {
					for(U32 i = 0; i < f.count && pos < els.size(); ++i)
						flatConsumeObject(fty, els, pos);
				} else
					flatConsumeObject(fty, els, pos);
				if(ty.strukt->isUnion)
					break;
			}
			return;
		}
		if(isArrayType(ty)) {
			CType el = arrayElem(ty);
			U32 cnt = ty.array->count;
			if(e->kind == ExprKind::InitList || (isCharType(el) && e->kind == ExprKind::StrLit)) {
				++pos;
				return;
			}
			for(U32 i = 0; i < cnt && pos < els.size(); ++i)
				flatConsumeObject(el, els, pos);
			return;
		}
		++pos; // scalar
	}

	U32 Emitter::flatArrayCount(CType elem, const List<Expr*>& els) {
		U32 pos = 0, n = 0;
		while(pos < els.size()) {
			flatConsumeObject(elem, els, pos);
			++n;
		}
		return n;
	}

	U32 Emitter::flexElemCount(const StructType* st, const Expr* init) {
		if(st->isUnion || st->fields.empty() || !init || init->kind != ExprKind::InitList)
			return 0;
		const Field& flex = st->fields.back();
		if(!flex.isArray || flex.count != 0)
			return 0;
		const List<Expr*>& els = init->args;
		U32 cur = 0, flexFi = st->fields.size() - 1;
		for(U32 i = 0; i < els.size();) {
			const Designator& des = init->designators[i];
			if(des.isSet) {
				I64 fi = fieldIndex(st, des);
				if(fi < 0)
					return 0;
				cur = (U32)fi;
			}
			while(cur < st->fields.size() && st->fields[cur].anonMember && !st->fields[cur].anonFirst)
				++cur;
			if(cur >= st->fields.size())
				break;
			if(cur == flexFi) {
				const Expr* el = els[i];
				if(el->kind == ExprKind::InitList && !des.next)
					return flatArrayCount(flex.type, el->args);
				List<Expr*> tail;
				for(U32 j = i; j < els.size(); ++j)
					tail.push_back(const_cast<Expr*>(els[j]));
				return flatArrayCount(flex.type, tail);
			}
			const Field& f = st->fields[cur];
			const Expr* el = els[i];
			if(el->kind == ExprKind::InitList ||
				 (f.isArray && isCharType(f.type) && el->kind == ExprKind::StrLit)) {
				++i;
			} else if(f.isArray) {
				for(U32 k = 0; k < f.count && i < els.size(); ++k)
					flatConsumeObject(f.type, els, i);
			} else
				flatConsumeObject(f.type, els, i);
			++cur;
		}
		return 0;
	}
} // namespace rat::cc
