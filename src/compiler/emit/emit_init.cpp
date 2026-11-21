#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		// elem and count of an array, false for any other type
		B32 arrayParts(CType t, CType& elem, U32& count) {
			if(!isArrayType(t))
				return false;
			elem = arrayElem(t);
			count = (U32)t.array->count;
			return true;
		}
	} // namespace detail

	const Expr* Emitter::peelAggregateCompound(const Expr* el) {
		if(el->kind != ExprKind::CompoundLit || isArrayType(el->compound.type))
			return el;
		return el->compound.init;
	}

	Expr* Emitter::wrapNested(const Designator* sub, const Expr* val) {
		Expr* list = arena.make<Expr>();
		list->kind = ExprKind::InitList;
		list->offset = val->offset;
		list->args.push_back(const_cast<Expr*>(val));
		list->designators.push_back(*sub);
		return list;
	}

	I64 Emitter::fieldIndex(const StructType* st, const Designator& des) {
		if(des.isIndex) {
			diag.fail("array designator in a struct initializer");
			return -1;
		}
		for(U32 i = 0; i < st->fields.size(); ++i)
			if(st->fields[i].name && *st->fields[i].name == *des.field)
				return (I64)i;
		diag.fail("no field named '" + *des.field + "' in the struct");
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
		if(e->args.size() != 1 || e->designators[0].isSet)
			return diag.fail("invalid initializer for a scalar");
		e = e->args[0];
		return true;
	}

	B32 Emitter::initListIsFlat(const Expr* init) {
		if(!init || init->kind != ExprKind::InitList)
			return false;
		for(U32 i = 0; i < init->designators.size(); ++i)
			if(init->designators[i].isSet)
				return false;
		return true;
	}

	// highest outer index + 1, from a dry run of the real walk
	U32 Emitter::initArrayCount(CType elem, const Expr* init) {
		DryRunDiag dryRun(diag);
		InitSink sink;
		U32 extent = 0;
		U32 pos = 0;
		B32 ok;
		// same walk registerGlobalArray picks
		if(isStruct(elem) && initListIsFlat(init))
			ok = initFlatArray(sink, 0, elem, UINT32_MAX, {init->args, pos}, &extent);
		else
			ok = initArrayInit(sink, 0, elem, UINT32_MAX, init, &extent);
		// the real walk must reach the element that failed
		if(!ok)
			extent = std::max(extent, 1u);
		return extent;
	}

	B32 Emitter::initArrayRow(InitSink& sink, U32 off, CType elem, const Expr* init, U32& i) {
		const List<Expr*>& els = init->args;
		const Designator& des = init->designators[i];
		Expr* row = wrapNested(des.next, els[i]);
		U32 rowLen = elem.array->count;
		U32 rowFree = (U32)des.next->index + 1 < rowLen ? rowLen - ((U32)des.next->index + 1) : 0;
		U32 j = i + 1;
		for(; j < els.size() && !init->designators[j].isSet && rowFree > 0; ++j) {
			row->args.push_back(els[j]);
			row->designators.push_back(Designator{});
			--rowFree;
		}
		if(!initArrayInit(sink, off, arrayElem(elem), elem.array->count, row))
			return false;
		i = j;
		return true;
	}

	B32 Emitter::initArrayInit(
			InitSink& sink, U32 base, CType elem, U32 count, const Expr* init, U32* extent) {
		if(init->kind != ExprKind::InitList)
			return diag.fail("expected a brace initializer for an array");
		U32 esz = byteSize(elem);
		const List<Expr*>& els = init->args;
		B32 elemIsChar = isCharType(elem);
		B32 bracedString = els.size() == 1 && els[0]->kind == ExprKind::StrLit && !els[0]->str.isWide;
		if(elemIsChar && bracedString && !init->designators[0].isSet) {
			if(extent)
				*extent = std::max(*extent, (U32)els[0]->str.bytes->size() + 1);
			return sink.charArray(base, elem, count, els[0]);
		}
		U32 cur = 0;
		for(U32 i = 0; i < els.size();) {
			const Designator& des = init->designators[i];
			if(des.isSet) {
				if(!des.isIndex)
					return diag.fail("field designator in an array initializer");
				cur = (U32)des.index;
			}
			if(cur >= count)
				return failTooManyInits();
			if(extent && cur >= *extent)
				*extent = cur + 1;
			U32 off = base + cur * esz;
			const Expr* el = els[i];
			if(des.isSet && des.next && isArrayType(elem) && des.next->isIndex) {
				if(!initArrayRow(sink, off, elem, init, i))
					return false;
				++cur;
				continue;
			}
			// the rest of the designator applies to a nested brace list
			if(des.isSet && des.next) {
				if(!isArrayType(elem) && !isStruct(elem))
					return diag.fail("designator selects a sub-object of a scalar element");
				el = wrapNested(des.next, el);
			}
			B32 braced = el->kind == ExprKind::InitList || el->kind == ExprKind::CompoundLit;
			if((isStruct(elem) || isArrayType(elem)) && !braced) {
				U32 first = i;
				if(!initFlatObject(sink, off, elem, {els, i, &init->designators}))
					return false;
				// a zero size element takes nothing
				if(i == first)
					return failTooManyInits();
				++cur;
				continue;
			}
			const Expr* peeled = peelAggregateCompound(el);
			if(isArrayType(elem)) {
				if(!initArrayInit(sink, off, arrayElem(elem), elem.array->count, peeled))
					return false;
			} else if(isStruct(elem)) {
				if(!initStructInit(sink, off, elem.strukt, peeled))
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
		if(cur >= fields.size())
			return diag.fail("too many initializers for the union");
		const Field& f = fields[cur];
		U32 off = base + f.offset;
		if(f.anonMember()) {
			U32 gfirst = cur;
			while(gfirst > 0 && !fields[gfirst].anonFirst())
				--gfirst;
			const StructType* g = anonGroupType(st, gfirst);
			U32 goff = base + fields[gfirst].offset;
			if(els.size() == 1 && !des.isSet && els[0]->kind == ExprKind::InitList)
				return initStructInit(sink, goff, g, els[0]);
			return initStructInit(sink, goff, g, init);
		}
		CType elem;
		U32 count = 0;
		B32 isArray = detail::arrayParts(f.type, elem, count);
		if(des.isSet && des.next) {
			if(isArray)
				return initArrayInit(sink, off, elem, count, wrapNested(des.next, els[0]));
			if(isStruct(f.type))
				return initStructInit(sink, off, f.type.strukt, wrapNested(des.next, els[0]));
			return diag.fail("designator selects a sub-object of a scalar union member");
		}
		if(isArray) {
			if(els[0]->kind == ExprKind::StrLit)
				return sink.charArray(off, elem, count, els[0]);
			if(els[0]->kind == ExprKind::InitList && els.size() == 1)
				return initArrayInit(sink, off, elem, count, els[0]);
			U32 pos = des.isSet ? 1 : 0;
			return initFlatArray(sink, off, elem, count, {els, pos});
		}
		if(isStruct(f.type)) {
			if(els[0]->kind == ExprKind::InitList && els.size() == 1)
				return initStructInit(sink, off, f.type.strukt, els[0]);
			U32 pos = des.isSet ? 1 : 0;
			return initFlatStruct(sink, off, f.type.strukt, {els, pos});
		}
		if(els.size() > 1)
			return diag.fail("too many initializers for the union");
		return sink.scalar(off, f.type, els[0]);
	}

	B32 Emitter::initStructInit(InitSink& sink,
															U32 base,
															const StructType* st,
															const Expr* init,
															U32 flexCount,
															U32* flexExtent) {
		if(init->kind != ExprKind::InitList)
			return diag.fail("expected a brace initializer for struct '" + st->tag + "'");
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
				while(cur < fields.size() && fields[cur].anonMember() && !fields[cur].anonFirst())
					++cur;
			}
			if(cur >= fields.size())
				return diag.fail("too many initializers for the struct");
			const Field& f = fields[cur];
			U32 off = base + f.offset;
			const Expr* el = els[i];
			if(!des.isSet && f.anonMember() && f.anonFirst()) {
				const StructType* g = anonGroupType(st, cur);
				if(el->kind == ExprKind::InitList) {
					++i;
					if(!initStructInit(sink, off, g, el))
						return false;
				} else if(!initFlatStruct(sink, off, g, {els, i, &init->designators}))
					return false;
				++cur; // the loop head skips the rest of the group
				continue;
			}
			CType elem;
			U32 fcount = 0;
			B32 isArray = detail::arrayParts(f.type, elem, fcount);
			U32* fieldExtent = nullptr;
			if(isArray && fcount == 0 && cur == fields.size() - 1) {
				fcount = flexCount;
				fieldExtent = flexExtent;
			}
			// the rest of the designator applies to a nested brace list
			if(des.isSet && des.next) {
				if(!isArray && !isStruct(f.type))
					return diag.fail("designator selects a sub-object of a scalar member");
				el = wrapNested(des.next, el);
			}
			if(isArray && el->kind != ExprKind::InitList && el->kind != ExprKind::StrLit) {
				if(!initFlatArray(sink, off, elem, fcount, {els, i}, fieldExtent))
					return false;
				++cur;
				continue;
			}
			B32 braced = el->kind == ExprKind::InitList || el->kind == ExprKind::CompoundLit;
			if(isStruct(f.type) && !braced) {
				if(!initFlatObject(sink, off, f.type, {els, i}))
					return false;
				++cur;
				continue;
			}
			if(isArray) {
				if(el->kind == ExprKind::StrLit) {
					if(fieldExtent)
						*fieldExtent =
								std::max(*fieldExtent, (U32)(el->str.bytes->size() / el->str.charSize) + 1);
					if(!sink.charArray(off, elem, fcount, el))
						return false;
				} else if(!initArrayInit(sink, off, elem, fcount, peelAggregateCompound(el), fieldExtent))
					return false;
			} else if(isStruct(f.type)) {
				if(!initStructInit(sink, off, f.type.strukt, peelAggregateCompound(el)))
					return false;
			} else if(f.isBitfield()) {
				if(!sink.bitfield(off, f.type, f.bitWidth, f.bitOffset, el))
					return false;
			} else if(!sink.scalar(off, f.type, el))
				return false;
			++cur;
			++i;
		}
		return true;
	}
} // namespace rat::cc
