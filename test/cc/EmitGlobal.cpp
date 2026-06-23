#include "Emit.h"

#include "IR/Function.h"
#include "IR/Module.h"

namespace rat::cc {
	String Emitter::internString(const Expr* e) {
		const String& bytes = *e->str.bytes;
		String name = "__ratcc_str" + std::to_string(strCounter++);
		List<U8> init;
		init.reserve(bytes.size() + 1);
		for (char c : bytes)
			init.push_back((U8)c);
		init.push_back(0);
		mod.createGlobal(name, mod.getArray(mod.getInt(8), (U32)init.size()),
										 true, std::move(init));
		return name;
	}

	B32 Emitter::internCompoundLiteral(const Expr* e, String& outSym) {
		CType ty = e->compound.type;
		const Expr* init = e->compound.init;
		List<Reloc> saved;
		saved.swap(relocs);
		B32 ok = true;
		String name = "__ratcc_cl" + std::to_string(strCounter++);

		U32 total = 0;
		I64 count = 0;
		if (e->compound.isArray) {
			if (e->compound.arrayLen)
				ok = evalConst(e->compound.arrayLen, count) && count > 0;
			else if (init->kind == ExprKind::StrLit)
				count = (I64)init->str.bytes->size() + 1;
			else if (init->kind == ExprKind::InitList)
				count = (I64)arrayInitOuterExtent(ty, init);
			if (ok && count <= 0)
				ok = false;
			total = (U32)count * byteSize(ty);
		} else if (isStruct(ty)) {
			total = ty.strukt->size;
		} else {
			total = byteSize(ty);
		}

		if (ok) {
			List<U8> img(total, 0);
			ImageSink sink(*this, img);
			if (e->compound.isArray && init->kind == ExprKind::StrLit)
				ok = sink.charArray(0, ty, (U32)count, init);
			else if (e->compound.isArray)
				ok = initArrayInit(sink, 0, ty, (U32)count, init);
			else if (isStruct(ty))
				ok = initStructInit(sink, 0, ty.strukt, init);
			else
				ok = sink.scalar(0, ty, init);
			if (ok)
				mod.createGlobal(name, mod.getArray(mod.getInt(8), total),
												 false, std::move(img), std::move(relocs));
		}

		relocs.swap(saved);
		if (!ok) {
			fail("invalid file-scope compound literal initializer");
			return false;
		}
		if (e->compound.isArray)
			globalArrays[name] = ty;
		else
			globals[name] = ty;
		outSym = name;
		return true;
	}

	void Emitter::bindArrayGlobal(const Declarator& d, const String& symbol,
																Function* fn, U32 count) {
		if (fn)
			declare(*d.name, Local{0, fn->global(symbol), d.type, true, true, count});
		else {
			globalArrays[*d.name] = d.type;
			globalArrayCounts[*d.name] = count;
		}
	}

	B32 Emitter::validateGlobalArrayLen(const Declarator& d, I64& count,
																			B32& haveLen) {
		haveLen = d.arrayLen != nullptr;
		count = 0;
		if (haveLen && (!evalConst(d.arrayLen, count) || count <= 0)) {
			failArrayCount();
			return false;
		}
		return true;
	}

	B32 Emitter::registerGlobalArrayOfArray(const Declarator& d,
																					const String& symbol, Function* fn) {
		B32 haveLen;
		I64 count;
		if (!validateGlobalArrayLen(d, count, haveLen))
			return false;
		if (!haveLen) {
			if (!d.init || d.init->kind != ExprKind::InitList) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			count = (I64)d.init->args.size();
		}
		U32 elemSize = byteSize(d.type);
		List<U8> img((U32)count * elemSize, 0);
		ImageSink sink(*this, img);
		if (d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
			return false;
		mod.createGlobal(symbol, mod.getArray(mod.getInt(8), (U32)img.size()),
										 false, std::move(img), std::move(relocs));
		bindArrayGlobal(d, symbol, fn, (U32)count);
		return true;
	}

	B32 Emitter::registerGlobalArrayOfStruct(const Declarator& d,
																					 const String& symbol, Function* fn) {
		U32 structSize = d.type.strukt->size;
		B32 haveLen;
		I64 count;
		if (!validateGlobalArrayLen(d, count, haveLen))
			return false;
		B32 flat = false;
		if (d.init && d.init->kind == ExprKind::InitList) {
			flat = true;
			for (U32 i = 0; i < d.init->designators.size(); ++i)
				if (d.init->designators[i].isSet) {
					flat = false;
					break;
				}
		}
		if (!haveLen) {
			if (!d.init || d.init->kind != ExprKind::InitList) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			if (flat) {
				count = (I64)flatArrayCount(d.type, d.init->args);
			} else
				count = (I64)arrayInitOuterExtent(d.type, d.init);
		}
		List<U8> img((U32)count * structSize, 0);
		ImageSink sink(*this, img);
		if (d.init) {
			if (flat) {
				U32 pos = 0;
				if (!initFlatArray(sink, 0, d.type, (U32)count, d.init->args, pos))
					return false;
			} else if (!initArrayInit(sink, 0, d.type, (U32)count, d.init))
				return false;
		}
		mod.createGlobal(symbol, mod.getArray(mod.getInt(8), (U32)img.size()),
										 false, std::move(img), std::move(relocs));
		bindArrayGlobal(d, symbol, fn, (U32)count);
		return true;
	}

	B32 Emitter::registerGlobalArrayOfScalar(const Declarator& d,
																					 const String& symbol, Function* fn) {
		U32 elemSize = byteSize(d.type);
		B32 haveLen;
		I64 count;
		if (!validateGlobalArrayLen(d, count, haveLen))
			return false;

		List<U8> init;

		if (d.init && d.init->kind == ExprKind::StrLit) {
			if (d.type.ptr != 0 || d.type.bits != 8) {
				failStringNeedsCharArray();
				return false;
			}
			const String& bytes = *d.init->str.bytes;
			if (!haveLen)
				count = (I64)bytes.size() + 1;
			for (I64 i = 0; i < count; ++i)
				init.push_back(i < (I64)bytes.size() ? (U8)bytes[(U32)i] : 0);
		} else if (d.init && d.init->kind == ExprKind::InitList) {
			const List<Expr*>& els = d.init->args;
			const List<Designator>& des = d.init->designators;
			List<I64> idx(els.size());
			I64 cur = 0, maxIdx = -1;
			for (U32 i = 0; i < els.size(); ++i) {
				if (des[i].isSet) {
					if (!des[i].isIndex) {
						failFieldInArray();
						return false;
					}
					cur = des[i].index;
				}
				idx[i] = cur;
				if (cur > maxIdx)
					maxIdx = cur;
				++cur;
			}
			if (!haveLen)
				count = maxIdx + 1;
			else if (maxIdx >= count) {
				failTooManyInits();
				return false;
			}
			if (count <= 0) {
				failArrayCount();
				return false;
			}
			init.assign((U32)count * elemSize, 0);
			ImageSink sink(*this, init);
			for (U32 i = 0; i < els.size(); ++i) {
				U32 base = (U32)idx[i] * elemSize;
				if (!sink.scalar(base, d.type, els[i]))
					return false;
			}
		} else if (d.init) {
			fail("invalid initializer for an array");
			return false;
		} else {
			if (!haveLen) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			init.assign((U32)count * elemSize, 0);
		}

		mod.createGlobal(symbol, mod.getArray(irType(d.type), (U32)count),
										 false, std::move(init), std::move(relocs));
		bindArrayGlobal(d, symbol, fn, (U32)count);
		return true;
	}

	B32 Emitter::registerGlobalArray(const Declarator& d, const String& symbol,
																	 Function* fn) {
		relocs.clear();
		if (isArrayType(d.type))
			return registerGlobalArrayOfArray(d, symbol, fn);
		if (isStruct(d.type))
			return registerGlobalArrayOfStruct(d, symbol, fn);
		return registerGlobalArrayOfScalar(d, symbol, fn);
	}

	B32 Emitter::registerGlobalStruct(const Declarator& d, const String& symbol,
																		Function* fn) {
		relocs.clear();
		const StructType* st = d.type.strukt;
		const Expr* sinit = d.init ? peelAggregateCompound(d.init) : nullptr;
		U32 flex = flexElemCount(st, sinit);
		U32 total = st->size;
		if (flex > 0)
			total += flex * byteSize(st->fields.back().type);
		List<U8> init(total, 0);

		flexCount = flex;
		if (sinit && sinit->kind == ExprKind::InitList) {
			ImageSink sink(*this, init);
			if (!initStructInit(sink, 0, st, sinit)) {
				flexCount = 0;
				return false;
			}
		} else if (sinit) {
			flexCount = 0;
			fail("invalid initializer for struct '" + *d.name + "'");
			return false;
		}
		flexCount = 0;

		mod.createGlobal(symbol, mod.getArray(mod.getInt(8), total),
										 false, std::move(init), std::move(relocs));
		if (fn)
			declare(*d.name, Local{0, fn->global(symbol), d.type, true, false});
		else
			globals[*d.name] = d.type;
		return true;
	}

	B32 Emitter::registerGlobalScalar(const Declarator& d, const String& symbol,
																		Function* fn) {
		relocs.clear();
		if (d.type.isVoid && !isPointer(d.type)) {
			fail("variable '" + *d.name + "' has incomplete type 'void'");
			return false;
		}
		const Expr* dinit = d.init;
		if (dinit && dinit->kind == ExprKind::InitList && dinit->args.size() == 1 &&
				dinit->args[0]->kind != ExprKind::InitList)
			dinit = dinit->args[0];
		U64 value = 0;
		B32 floatImg = false;
		List<U8> fbytes;
		if (dinit) {
			if (isFloating(d.type)) {
				long double dv = 0;
				if (!evalFloatConst(dinit, dv)) {
					fail("initializer for '" + *d.name +
							 "' is not a constant expression");
					return false;
				}
				encodeFloatBytes(d.type, dv, fbytes);
				floatImg = true;
			} else {
				I64 iv = 0;
				if (!evalConst(dinit, iv)) {
					String sym;
					I64 add = 0;
					B32 isIntScalar = !isPointer(d.type) && !isFloating(d.type) &&
														!isAggregate(d.type) && !isVoidType(d.type);
					B32 fits =
							isPointer(d.type) || (isIntScalar && byteSize(d.type) >= 8);
					if (fits && evalAddrConst(dinit, sym, add)) {
						relocs.push_back(Reloc{0, sym, add});
					} else {
						fail("initializer for '" + *d.name +
								 "' is not a constant expression");
						return false;
					}
				}
				value = (U64)iv;
			}
		}
		U32 bytes = byteSize(d.type);
		List<U8> init;
		if (floatImg) {
			init = std::move(fbytes);
			init.resize(bytes, 0);
		} else {
			for (U32 i = 0; i < bytes; ++i)
				init.push_back((U8)(value >> (8 * i)));
		}
		mod.createGlobal(symbol, irType(d.type), false, std::move(init),
										 std::move(relocs));
		if (fn)
			declare(*d.name, Local{0, fn->global(symbol), d.type, true, false});
		else
			globals[*d.name] = d.type;
		return true;
	}

	B32 Emitter::registerGlobals(const TransUnit& unit) {
		List<const Declarator*> order;
		List<B32> needsStorage;
		Map<String, U32> seen;
		for (const Stmt* decl : unit.globals) {
			for (const Declarator& d : decl->decls) {
				B32 defines = d.init != nullptr || !d.isExtern;
				auto it = seen.find(*d.name);
				if (it == seen.end()) {
					seen[*d.name] = (U32)order.size();
					order.push_back(&d);
					needsStorage.push_back(defines);
					continue;
				}
				const Declarator*& prev = order[it->second];
				if (d.init && prev->init) {
					fail("redefinition of '" + *d.name + "'");
					return false;
				}
				if (d.init && !prev->init)
					prev = &d;
				if (defines)
					needsStorage[it->second] = true;
			}
		}
		for (U32 gi = 0; gi < order.size(); ++gi) {
			const Declarator& d = *order[gi];
			if (!needsStorage[gi]) {
				if (d.isArray) {
					globalArrays[*d.name] = d.type;
					I64 count = 0;
					if (d.arrayLen)
						evalConst(d.arrayLen, count);
					globalArrayCounts[*d.name] = (U32)count;
				} else {
					globals[*d.name] = d.type;
				}
				continue;
			}
			if (d.isArray) {
				if (!registerGlobalArray(d, *d.name, nullptr))
					return false;
				continue;
			}
			if (isStruct(d.type)) {
				if (!registerGlobalStruct(d, *d.name, nullptr))
					return false;
				continue;
			}
			if (!registerGlobalScalar(d, *d.name, nullptr))
				return false;
		}
		return true;
	}
} // namespace rat::cc
