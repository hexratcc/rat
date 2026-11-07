#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		// memory beats integer, integer beats x87 and sse, x87 with sse is memory
		AbiClass mergeAbiClass(AbiClass a, AbiClass b) {
			if(a == AbiClass::None || a == b)
				return b;
			if(b == AbiClass::None)
				return a;
			if(a == AbiClass::Memory || b == AbiClass::Memory)
				return AbiClass::Memory;
			if(a == AbiClass::Integer || b == AbiClass::Integer)
				return AbiClass::Integer;
			return AbiClass::Memory;
		}

		void classifyBits(AbiClass (&cls)[2], U64 bitOffset, U64 bits, AbiClass c) {
			for(U64 k = bitOffset / 64; k <= (bitOffset + bits - 1) / 64; ++k)
				cls[k] = mergeAbiClass(cls[k], c);
		}
	} // namespace detail

	// sysv psABI 3.2.3, win64 passes only sizes 1, 2, 4 and 8 in a register
	AbiRegs Emitter::abiRegs(CType t) const {
		U64 size = byteSize(t);
		AbiRegs regs;
		if(lay.win64VaList) {
			if(size == 1 || size == 2 || size == 4 || size == 8)
				regs = {1, {AbiClass::Integer}};
			return regs;
		}
		if(size > 16)
			return regs;
		// an empty aggregate travels as one eightbyte that takes no register
		if(size == 0)
			return {1, {}};
		classifyEightbytes(t, 0, regs.cls);
		for(AbiClass c : regs.cls)
			if(c == AbiClass::Memory || c == AbiClass::X87)
				return {};
		regs.count = (U8)((size + 7) / 8);
		return regs;
	}

	// a long double, alone or as the parts of a complex, returns on the x87 stack
	AbiRegs Emitter::returnRegs(CType t) const {
		if(lay.win64VaList)
			return abiRegs(t);
		AbiRegs x87 = {1, {AbiClass::X87, AbiClass::X87}};
		if(isComplexType(t) && complexElem(t).bits == 128) {
			x87.count = 2;
			return x87;
		}
		AbiClass cls[2] = {};
		if(byteSize(t) == 16)
			classifyEightbytes(t, 0, cls);
		if(cls[0] == AbiClass::X87 && cls[1] == AbiClass::X87)
			return x87;
		return abiRegs(t);
	}

	void Emitter::classifyEightbytes(CType t, U64 offset, AbiClass (&cls)[2]) const {
		U64 size = byteSize(t);
		if(size == 0)
			return;
		if(offset % alignOf(t) != 0) {
			cls[0] = AbiClass::Memory;
			return;
		}
		if(isArrayType(t)) {
			U64 elemSize = byteSize(t.array->elem);
			for(U64 i = 0; i < t.array->count; ++i)
				classifyEightbytes(t.array->elem, offset + i * elemSize, cls);
		} else if(isComplexType(t)) {
			CType elem = complexElem(t);
			classifyEightbytes(elem, offset, cls);
			classifyEightbytes(elem, offset + byteSize(elem), cls);
		} else if(isStruct(t)) {
			classifyFields(t.strukt, offset, cls);
		} else if(isFloating(t)) {
			AbiClass c = t.bits == 128 ? AbiClass::X87 : AbiClass::Sse;
			detail::classifyBits(cls, offset * 8, size * 8, c);
		} else {
			detail::classifyBits(cls, offset * 8, size * 8, AbiClass::Integer);
		}
	}

	// union members overlap at their offsets, a flexible array has no elements
	void Emitter::classifyFields(const StructType* st, U64 offset, AbiClass (&cls)[2]) const {
		for(const Field& f : st->fields) {
			U64 at = offset + f.offset;
			if(f.isBitfield()) {
				detail::classifyBits(cls, at * 8 + f.bitOffset, f.bitWidth, AbiClass::Integer);
				continue;
			}
			U64 count = f.isArray() ? f.count : 1;
			for(U64 i = 0; i < count; ++i)
				classifyEightbytes(f.type, at + i * byteSize(f.type), cls);
		}
	}

	// an aggregate takes registers only when all of its eightbytes fit, else it goes on the stack
	AbiRegs Emitter::takeArgRegs(CType t, ArgRegs& free) const {
		AbiRegs regs;
		U32 gp = 0;
		U32 sse = 0;
		if(isAggregate(t)) {
			regs = abiRegs(t);
			for(U32 k = 0; k < regs.count; ++k) {
				gp += regs.cls[k] == AbiClass::Integer;
				sse += regs.cls[k] == AbiClass::Sse;
			}
		} else if(!isFloating(t)) {
			gp = 1;
		} else if(t.bits != 128) {
			sse = 1;
		}
		if(lay.win64VaList)
			return regs;
		if(gp > free.gp || sse > free.sse)
			return {};
		free.gp -= gp;
		free.sse -= sse;
		return regs;
	}

	// a memory class aggregate return takes a register for the hidden pointer
	Emitter::ArgRegs Emitter::freeArgRegs(CType ret) const {
		ArgRegs free;
		if(isAggregate(ret) && returnRegs(ret).count == 0)
			--free.gp;
		return free;
	}

	// win64 passes a pointer to the copy instead
	void Emitter::markByval(CallAbi& abi, U32 index, CType t) const {
		if(lay.win64VaList)
			return;
		abi.byval.resize(index + 1);
		abi.byval[index] = {(U32)byteSize(t), alignOf(t)};
	}

	Type* Emitter::eightbyteType(AbiClass c) {
		return c == AbiClass::Sse ? mod.getFloat(64) : mod.getInt(64);
	}

	// whole eightbytes, so register halves can be stored and loaded
	Node* Emitter::eightbyteSlot(Function& fn, CType t) {
		U32 size = (U32)((byteSize(t) + 7) & ~7ull);
		return fn.alloc(byteArrayType(size), alignOf(t));
	}
} // namespace rat::cc
