#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::initFlatObject(InitSink& sink, U32 base, CType ty, const FlatList& src) {
		if(src.pos >= src.els.size())
			return true;
		const Expr* e = src.els[src.pos];
		if(isStruct(ty)) {
			const Expr* peeled = peelAggregateCompound(e);
			if(peeled != e || e->kind == ExprKind::InitList) {
				++src.pos;
				return initStructInit(sink, base, ty.strukt, peeled);
			}
			CType et;
			if(typeOf(e, et) && isStruct(et) && et.strukt == ty.strukt) {
				++src.pos;
				return sink.structCopy(base, ty, e);
			}
			return initFlatStruct(sink, base, ty.strukt, src);
		}
		if(isArrayType(ty))
			return initFlatArrayObject(sink, base, arrayElem(ty), ty.array->count, src);
		++src.pos;
		return sink.scalar(base, ty, e);
	}

	B32 Emitter::initFlatStruct(InitSink& sink, U32 base, const StructType* st, const FlatList& src) {
		for(U32 fi = 0; fi < st->fields.size() && src.pos < src.els.size(); ++fi) {
			if(fi != 0 && src.des && (*src.des)[src.pos].isSet)
				break;
			const Field& f = st->fields[fi];
			if(f.anonMember() && !f.anonFirst())
				continue;
			if(f.isArray()) {
				if(!initFlatArrayObject(sink, base + f.offset, f.type, f.count, src))
					return false;
			} else if(f.isBitfield()) {
				if(!sink.bitfield(base + f.offset, f.type, f.bitWidth, f.bitOffset, src.els[src.pos]))
					return false;
				++src.pos;
			} else if(!initFlatObject(sink, base + f.offset, f.type, src)) {
				return false;
			}
			if(st->isUnion)
				break;
		}
		return true;
	}

	B32 Emitter::initFlatArrayObject(
			InitSink& sink, U32 base, CType elem, U32 count, const FlatList& src) {
		const Expr* e = src.els[src.pos];
		if(e->kind == ExprKind::InitList) {
			++src.pos;
			return initArrayInit(sink, base, elem, count, e);
		}
		if(isCharType(elem) && e->kind == ExprKind::StrLit) {
			++src.pos;
			return sink.charArray(base, elem, count, e);
		}
		return initFlatArray(sink, base, elem, count, src);
	}

	B32 Emitter::initFlatArray(
			InitSink& sink, U32 base, CType elem, U32 count, const FlatList& src, U32* extent) {
		U32 esz = byteSize(elem);
		for(U32 i = 0; i < count && src.pos < src.els.size(); ++i) {
			if(i != 0 && src.des && (*src.des)[src.pos].isSet)
				break;
			if(extent && i >= *extent)
				*extent = i + 1;
			U32 first = src.pos;
			if(!initFlatObject(sink, base + i * esz, elem, src))
				return false;
			// a zero size element takes nothing
			if(src.pos == first)
				break;
		}
		return true;
	}

	B32 Emitter::StoreSink::scalar(U32 off, CType dt, const Expr* e) {
		LValue lv;
		lv.type = dt;
		return store(off, lv, e);
	}
	B32 Emitter::StoreSink::bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) {
		LValue lv;
		lv.type = dt;
		lv.isBitfield = true;
		lv.bitWidth = width;
		lv.bitOffset = bitOff;
		return store(off, lv, e);
	}
	B32 Emitter::StoreSink::store(U32 off, LValue lv, const Expr* e) {
		B32 skip = false;
		if(!emit.unwrapScalarInit(e, skip))
			return false;
		if(skip)
			return true;
		Value v = emit.emitExpr(fn, e);
		if(!v.node)
			return false;
		Node* val = emit.convert(fn, v.node, v.type, lv.type);
		lv.addr = emit.offsetPtr(fn, slot, off);
		emit.storeLValue(fn, lv, val);
		return true;
	}
	B32 Emitter::StoreSink::charArray(U32 base, CType elem, U32 count, const Expr* e) {
		if(!isCharType(elem))
			return emit.failStringNeedsCharArray();
		const String& bytes = *e->str.bytes;
		Type* i8 = emit.mod.getInt(8);
		U32 n = bytes.size() < count ? (U32)bytes.size() : count;
		for(U32 i = 0; i < n; ++i)
			fn.store(emit.offsetPtr(fn, slot, base + i), fn.constInt(i8, (U8)bytes[i]));
		return true;
	}
	B32 Emitter::StoreSink::structCopy(U32 off, CType ty, const Expr* e) {
		Value v = emit.emitExpr(fn, e);
		if(!v.node)
			return false;
		emit.emitMemCopy(fn, emit.offsetPtr(fn, slot, off), v.node, ty.strukt->size);
		return true;
	}

	void Emitter::encodeFloatBytes(CType dt, F80 v, List<U8>& out) {
		F32 f = (F32)v;
		F64 d = (F64)v;
		const U8* p = (const U8*)&v; // x86 extended precision, 10 bytes padded to 16
		if(dt.bits == 32)
			p = (const U8*)&f;
		else if(dt.bits == 64)
			p = (const U8*)&d;
		U32 size = byteSize(dt);
		for(U32 i = 0; i < size; ++i)
			out.push_back(i < 10 ? p[i] : (U8)0);
	}

	namespace detail {
		void writeLittleEndian(List<U8>& out, U32 off, U64 value, U32 size) {
			for(U32 b = 0; b < size; ++b)
				out[off + b] = (U8)(value >> (8 * b));
		}
	} // namespace detail

	// constant scalar -> bytes at off, or an address reloc there, no diagnostics of its own
	B32 Emitter::writeConstScalar(List<U8>& img, U32 off, CType dt, const Expr* e, B32 addrFits) {
		if(isFloating(dt)) {
			F80 fv = 0;
			if(!evalFloatConst(e, fv))
				return false;
			List<U8> bytes;
			encodeFloatBytes(dt, fv, bytes);
			for(U32 b = 0; b < bytes.size() && off + b < img.size(); ++b)
				img[off + b] = bytes[b];
			return true;
		}
		I64 iv = 0;
		if(evalConst(e, iv)) {
			detail::writeLittleEndian(img, off, (U64)iv, byteSize(dt));
			return true;
		}
		String sym;
		I64 add = 0;
		if(!addrFits || !evalAddrConst(e, sym, add))
			return false;
		data.setReloc(off, sym, add);
		return true;
	}

	B32 Emitter::ImageSink::scalar(U32 off, CType dt, const Expr* e) {
		B32 skip = false;
		if(!emit.unwrapScalarInit(e, skip))
			return false;
		if(skip)
			return true;
		if(!emit.writeConstScalar(img, off, dt, e, isPointer(dt)))
			return emit.failNonConstInit();
		return true;
	}

	B32 Emitter::ImageSink::bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) {
		I64 v = 0;
		if(!emit.evalConst(e, v))
			return emit.failNonConstInit();
		U32 sz = emit.byteSize(dt);
		U64 maskBits = width >= 64 ? ~0ull : ((1ull << width) - 1);
		U64 placed = ((U64)v & maskBits) << bitOff;
		U64 unit = 0;
		for(U32 b = 0; b < sz; ++b)
			unit |= (U64)img[off + b] << (8 * b);
		unit |= placed;
		detail::writeLittleEndian(img, off, unit, sz);
		return true;
	}

	B32 Emitter::ImageSink::charArray(U32 base, CType elem, U32 count, const Expr* e) {
		if(!isCharType(elem))
			return emit.failStringNeedsCharArray();
		const String& bytes = *e->str.bytes;
		U32 n = bytes.size() < count ? (U32)bytes.size() : count;
		for(U32 i = 0; i < n; ++i)
			img[base + i] = (U8)bytes[i];
		return true;
	}

	B32 Emitter::ImageSink::structCopy(U32, CType, const Expr*) { return emit.failNonConstInit(); }

	B32 Emitter::ExtentSink::scalar(U32, CType, const Expr*) { return true; }
	B32 Emitter::ExtentSink::bitfield(U32, CType, U32, U32, const Expr*) { return true; }
	B32 Emitter::ExtentSink::charArray(U32, CType, U32, const Expr*) { return true; }
	B32 Emitter::ExtentSink::structCopy(U32, CType, const Expr*) { return true; }

	const StructType* Emitter::anonGroupType(const StructType* st, U32 firstIdx) {
		StructType* g = arena.make<StructType>();
		g->isUnion = st->fields[firstIdx].anonUnion();
		U32 baseOff = st->fields[firstIdx].offset;
		for(U32 i = firstIdx; i < st->fields.size(); ++i) {
			const Field& f = st->fields[i];
			if(i != firstIdx && !(f.anonMember() && !f.anonFirst()))
				break;
			Field nf = f;
			nf.offset = f.offset - baseOff;
			nf.set(Field::AnonMember, false);
			nf.set(Field::AnonFirst, false);
			g->fields.push_back(nf);
		}
		g->size = 0;
		g->align = 1;
		return g;
	}

	// flexible array member length, from a dry run of the real walk
	U32 Emitter::flexElemCount(const StructType* st, const Expr* init) {
		if(!init)
			return 0;
		DryRunDiag dryRun(diag);
		ExtentSink sink;
		U32 extent = 0;
		U32 savedFlexCount = data.flexCount;
		data.flexCount = UINT32_MAX;
		initStructInit(sink, 0, st, init, &extent);
		data.flexCount = savedFlexCount;
		return extent;
	}
} // namespace rat::cc
