#include "Emit.h"

namespace rat::cc {
	const Expr* Emitter::peelAggregateCompound(const Expr* el) {
		if(el->kind == ExprKind::CompoundLit && !el->compound.isArray && el->compound.init &&
			 el->compound.init->kind == ExprKind::InitList)
			return el->compound.init;
		return el;
	}

	const Expr* Emitter::wrapNested(const Designator* sub, const Expr* val) {
		Expr* list = arena.make<Expr>();
		list->kind = ExprKind::InitList;
		list->offset = val->offset;
		Designator head = *sub;
		list->args.push_back(const_cast<Expr*>(val));
		list->designators.push_back(head);
		return list;
	}

	I64 Emitter::fieldIndex(const StructType* st, const Designator& des) {
		if(des.isIndex) {
			fail("array designator in a struct initializer");
			return -1;
		}
		for(U32 i = 0; i < st->fields.size(); ++i)
			if(st->fields[i].name && *st->fields[i].name == *des.field)
				return (I64)i;
		fail("no field named '" + *des.field + "' in the struct");
		return -1;
	}

	B32 Emitter::unwrapScalarInit(const Expr*& e, B32& skip) {
		skip = false;
		if(e->kind != ExprKind::InitList)
			return true;
		if(e->args.empty()) {
			skip = true;
			return true;
		}
		if(e->args.size() != 1 || e->designators[0].isSet) {
			failScalarInit();
			return false;
		}
		e = e->args[0];
		return true;
	}

	B32 Emitter::storeScalar(Function& fn, Node* slot, U32 off, CType dt, const Expr* e) {
		B32 skip = false;
		if(!unwrapScalarInit(e, skip))
			return false;
		if(skip)
			return true;
		Value v = emitExpr(fn, e);
		if(!v.node)
			return false;
		Node* val = convert(fn, v.node, v.type, dt);
		fn.store(offsetPtr(fn, slot, off), val);
		return true;
	}

	B32 Emitter::storeCharArray(
			Function& fn, Node* slot, U32 base, CType elem, U32 count, const Expr* e) {
		if(!isCharType(elem)) {
			failStringNeedsCharArray();
			return false;
		}
		const String& bytes = *e->str.bytes;
		Type* i8 = mod.getInt(8);
		U32 n = bytes.size() < count ? (U32)bytes.size() : count;
		for(U32 i = 0; i < n; ++i)
			fn.store(offsetPtr(fn, slot, base + i), fn.constInt(i8, (U8)bytes[i]));
		return true;
	}

	U32 Emitter::arrayInitOuterExtent(CType elem, const Expr* init) {
		const List<Expr*>& els = init->args;
		U32 rowLen = elem.array ? elem.array->count : 0;
		U32 cur = 0, maxIdx = 0;
		B32 any = false;
		for(U32 i = 0; i < els.size();) {
			const Designator& des = init->designators[i];
			if(des.isSet && des.isIndex)
				cur = (U32)des.index;
			any = true;
			if(cur >= maxIdx)
				maxIdx = cur;
			const Expr* el = els[i];
			if(des.isSet && des.next && isArrayType(elem) && des.next->isIndex) {
				U32 rowFree = (U32)des.next->index + 1 < rowLen ? rowLen - ((U32)des.next->index + 1) : 0;
				U32 j = i + 1;
				for(; j < els.size() && !init->designators[j].isSet && rowFree > 0; ++j, --rowFree)
					;
				i = j;
			} else if(isArrayType(elem) && el->kind != ExprKind::InitList &&
								el->kind != ExprKind::CompoundLit && el->kind != ExprKind::StrLit) {
				U32 rowFree = rowLen;
				U32 j = i;
				for(; j < els.size() && rowFree > 0 && (j == i || !init->designators[j].isSet);
						++j, --rowFree)
					;
				i = j;
			} else {
				++i;
			}
			++cur;
		}
		return any ? maxIdx + 1 : 0;
	}

	B32 Emitter::resolveArrayIndices(const List<Expr*>& els,
																	 const List<Designator>& des,
																	 List<I64>& idx,
																	 I64& maxIdx) {
		I64 cur = 0;
		maxIdx = -1;
		for(U32 i = 0; i < els.size(); ++i) {
			if(des[i].isSet) {
				if(!des[i].isIndex) {
					failFieldInArray();
					return false;
				}
				cur = des[i].index;
			}
			idx[i] = cur;
			if(cur > maxIdx)
				maxIdx = cur;
			++cur;
		}
		return true;
	}

	B32 Emitter::initArrayRow(InitSink& sink,
														U32 off,
														CType elem,
														const Expr* init,
														const Designator& des,
														U32& i,
														U32& cur) {
		const List<Expr*>& els = init->args;
		const Expr* el = els[i];
		Expr* row = arena.make<Expr>();
		row->kind = ExprKind::InitList;
		row->offset = el->offset;
		row->args.push_back(const_cast<Expr*>(el));
		row->designators.push_back(*des.next);
		U32 rowLen = elem.array->count;
		U32 rowFree = (U32)des.next->index + 1 < rowLen ? rowLen - ((U32)des.next->index + 1) : 0;
		U32 j = i + 1;
		for(; j < els.size() && !init->designators[j].isSet && rowFree > 0; ++j, --rowFree) {
			row->args.push_back(els[j]);
			row->designators.push_back(Designator{});
		}
		if(!initArrayInit(sink, off, arrayElem(elem), elem.array->count, row))
			return false;
		++cur;
		i = j;
		return true;
	}

	B32 Emitter::initArrayInit(InitSink& sink, U32 base, CType elem, U32 count, const Expr* init) {
		if(init->kind != ExprKind::InitList) {
			fail("expected a brace initializer for an array");
			return false;
		}
		U32 esz = byteSize(elem);
		const List<Expr*>& els = init->args;
		B32 elemIsChar = isCharType(elem);
		U32 cur = 0;
		for(U32 i = 0; i < els.size();) {
			const Designator& des = init->designators[i];
			if(des.isSet) {
				if(!des.isIndex) {
					failFieldInArray();
					return false;
				}
				cur = (U32)des.index;
			}
			if(cur >= count) {
				failTooManyInits();
				return false;
			}
			U32 off = base + cur * esz;
			const Expr* el = els[i];
			if(des.isSet && des.next && isArrayType(elem) && des.next->isIndex) {
				if(!initArrayRow(sink, off, elem, init, des, i, cur))
					return false;
				continue;
			}
			if(des.isSet && des.next) {
				if(isArrayType(elem)) {
					if(!initArrayInit(
								 sink, off, arrayElem(elem), elem.array->count, wrapNested(des.next, el)))
						return false;
				} else if(isStruct(elem)) {
					if(!initStructInit(sink, off, elem.strukt, wrapNested(des.next, el)))
						return false;
				} else {
					fail("designator selects a sub-object of a scalar element");
					return false;
				}
				++cur;
				++i;
				continue;
			}
			if((isStruct(elem) || isArrayType(elem)) && el->kind != ExprKind::InitList &&
				 el->kind != ExprKind::CompoundLit &&
				 !(isArrayType(elem) && elemIsChar && el->kind == ExprKind::StrLit)) {
				if(!initFlatObject(sink, off, elem, els, i, &init->designators))
					return false;
				++cur;
				continue;
			}
			if(isArrayType(elem)) {
				if(elemIsChar && el->kind == ExprKind::StrLit) {
					if(!sink.charArray(off, arrayElem(elem), elem.array->count, el))
						return false;
				} else if(!initArrayInit(
											sink, off, arrayElem(elem), elem.array->count, peelAggregateCompound(el)))
					return false;
			} else if(isStruct(elem)) {
				if(!initStructInit(sink, off, elem.strukt, peelAggregateCompound(el)))
					return false;
			} else if(!sink.scalar(off, elem, el))
				return false;
			++cur;
			++i;
		}
		return true;
	}

	B32 Emitter::initUnionInit(InitSink& sink, U32 base, const StructType* st, const Expr* init) {
		const List<Field>& fields = st->fields;
		const List<Expr*>& els = init->args;
		if(els.empty())
			return true;
		U32 cur = 0;
		const Designator& des = init->designators[0];
		if(des.isSet) {
			I64 fi = fieldIndex(st, des);
			if(fi < 0)
				return false;
			cur = (U32)fi;
		}
		if(cur >= fields.size()) {
			fail("too many initializers for the union");
			return false;
		}
		const Field& f = fields[cur];
		U32 off = base + f.offset;
		if(f.anonMember) {
			U32 gfirst = cur;
			while(gfirst > 0 && !fields[gfirst].anonFirst)
				--gfirst;
			const StructType* g = anonGroupType(st, gfirst);
			U32 goff = base + fields[gfirst].offset;
			if(els.size() == 1 && !des.isSet && els[0]->kind == ExprKind::InitList)
				return initStructInit(sink, goff, g, els[0]);
			return initStructInit(sink, goff, g, init);
		}
		if(des.isSet && des.next) {
			if(f.isArray)
				return initArrayInit(sink, off, f.type, f.count, wrapNested(des.next, els[0]));
			if(isStruct(f.type))
				return initStructInit(sink, off, f.type.strukt, wrapNested(des.next, els[0]));
			fail("designator selects a sub-object of a scalar union member");
			return false;
		}
		if(f.isArray) {
			if(els[0]->kind == ExprKind::StrLit)
				return sink.charArray(off, f.type, f.count, els[0]);
			if(els[0]->kind == ExprKind::InitList && els.size() == 1)
				return initArrayInit(sink, off, f.type, f.count, els[0]);
			U32 pos = des.isSet ? 1 : 0;
			return initFlatArray(sink, off, f.type, f.count, els, pos);
		}
		if(isStruct(f.type)) {
			if(els[0]->kind == ExprKind::InitList && els.size() == 1)
				return initStructInit(sink, off, f.type.strukt, els[0]);
			U32 pos = des.isSet ? 1 : 0;
			return initFlatStruct(sink, off, f.type.strukt, els, pos);
		}
		if(els.size() > 1) {
			fail("too many initializers for the union");
			return false;
		}
		return sink.scalar(off, f.type, els[0]);
	}

	B32 Emitter::initStructInit(InitSink& sink, U32 base, const StructType* st, const Expr* init) {
		if(init->kind != ExprKind::InitList) {
			fail("expected a brace initializer for struct '" + st->tag + "'");
			return false;
		}
		if(st->isUnion)
			return initUnionInit(sink, base, st, init);
		const List<Field>& fields = st->fields;
		const List<Expr*>& els = init->args;
		U32 cur = 0;
		for(U32 i = 0; i < els.size();) {
			const Designator& des = init->designators[i];
			if(des.isSet) {
				I64 fi = fieldIndex(st, des);
				if(fi < 0)
					return false;
				cur = (U32)fi;
			} else {
				while(cur < fields.size() && fields[cur].anonMember && !fields[cur].anonFirst)
					++cur;
			}
			if(cur >= fields.size()) {
				fail("too many initializers for the struct");
				return false;
			}
			const Field& f = fields[cur];
			U32 off = base + f.offset;
			const Expr* el = els[i];
			if(!des.isSet && f.anonMember && f.anonFirst) {
				const StructType* g = anonGroupType(st, cur);
				if(el->kind == ExprKind::InitList) {
					++i;
					if(!initStructInit(sink, off, g, el))
						return false;
				} else if(!initFlatStruct(sink, off, g, els, i, &init->designators))
					return false;
				for(++cur; cur < fields.size() && fields[cur].anonMember && !fields[cur].anonFirst; ++cur)
					;
				continue;
			}
			U32 fcount = f.count;
			if(f.isArray && f.count == 0 && cur == fields.size() - 1)
				fcount = flexCount;
			if(des.isSet && des.next) {
				if(f.isArray) {
					if(!initArrayInit(sink, off, f.type, fcount, wrapNested(des.next, el)))
						return false;
				} else if(isStruct(f.type)) {
					if(!initStructInit(sink, off, f.type.strukt, wrapNested(des.next, el)))
						return false;
				} else {
					fail("designator selects a sub-object of a scalar member");
					return false;
				}
				++cur;
				++i;
				continue;
			}
			if(f.isArray && el->kind != ExprKind::InitList && el->kind != ExprKind::StrLit) {
				if(!initFlatArray(sink, off, f.type, fcount, els, i))
					return false;
				++cur;
				continue;
			}
			if(!f.isArray && isStruct(f.type) && el->kind != ExprKind::InitList &&
				 el->kind != ExprKind::CompoundLit) {
				if(!initFlatObject(sink, off, f.type, els, i))
					return false;
				++cur;
				continue;
			}
			if(f.isArray) {
				if(el->kind == ExprKind::StrLit) {
					if(!sink.charArray(off, f.type, fcount, el))
						return false;
				} else if(!initArrayInit(sink, off, f.type, fcount, peelAggregateCompound(el)))
					return false;
			} else if(isStruct(f.type)) {
				if(!initStructInit(sink, off, f.type.strukt, peelAggregateCompound(el)))
					return false;
			} else if(f.isBitfield) {
				if(!sink.bitfield(off, f.type, f.bitWidth, f.bitOffset, el))
					return false;
			} else if(!sink.scalar(off, f.type, el))
				return false;
			++cur;
			++i;
		}
		return true;
	}

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
			long double ld = v;
			const U8* p = (const U8*)&ld;
			U32 n = byteSize(dt); // 16
			for(U32 i = 0; i < n; ++i)
				out.push_back(i < sizeof(ld) ? p[i] : (U8)0);
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
