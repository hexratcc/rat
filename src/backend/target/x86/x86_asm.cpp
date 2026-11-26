#include "target/x86/x86_asm.h"

#include "byte_io.h"

namespace rat {
	const X86CallConv& x86CallConv(OS os) { return os == OS::Windows ? abi::kWin64 : abi::kSysV; }

	X86ArgAssigner::Loc X86ArgAssigner::next(Kind k) {
		if(k == Kind::X87)
			return nextMem(16, 16);
		if(conv.sharedSlots) {
			Loc l{slot < conv.gpArgCount ? (I32)slot : -1, stackBytes};
			if(l.reg < 0)
				stackBytes += 8;
			++slot;
			return l;
		}
		U32& used = k == Kind::Sse ? sseUsed : gpUsed;
		U32 cap = k == Kind::Sse ? conv.sseArgCount : conv.gpArgCount;
		if(used < cap)
			return {(I32)used++, 0};
		Loc l{-1, stackBytes};
		stackBytes += 8;
		return l;
	}

	X86ArgAssigner::Loc X86ArgAssigner::nextMem(U32 bytes, U32 align) {
		U32 slotAlign = std::clamp(align, 8u, 16u);
		stackBytes = (U32)alignUp(stackBytes, slotAlign);
		Loc l{-1, stackBytes};
		stackBytes += (U32)alignUp(bytes, 8);
		return l;
	}

	Mem indexedMem(Reg base, Reg index, U32 scaleLog2, I32 disp) {
		return {base, disp, index, scaleLog2, true};
	}

	U32 Asm::here() const { return (U32)code.size(); }
	void Asm::b(U8 v) { code.push_back(v); }
	void Asm::d32(U32 v) { le::put32(code, v); }
	void Asm::d64(U64 v) { le::put64(code, v); }

	void Asm::imm(I64 v, U32 bytes) {
		if(bytes == 1)
			b((U8)v);
		else if(bytes == 2)
			le::put16(code, (U16)v);
		else
			d32((U32)(I32)v);
	}

	// the recommended nop forms of 1 to 9 bytes (Intel SDM, NOP)
	void Asm::alignNop(U32 a) {
		static const U8 kNop[9][9] = {
				{0x90},
				{0x66, 0x90},
				{0x0f, 0x1f, 0x00},
				{0x0f, 0x1f, 0x40, 0x00},
				{0x0f, 0x1f, 0x44, 0x00, 0x00},
				{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
				{0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
				{0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
				{0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
		};
		U32 pad = (a - here() % a) % a;
		while(pad) {
			U32 len = std::min(pad, 9u);
			code.insert(code.end(), kNop[len - 1], kNop[len - 1] + len);
			pad -= len;
		}
	}

	// prefix, rex, escape and opcode bytes
	void Asm::lead(Enc e, U32 reg, U32 index, U32 base) {
		if(e.pfx)
			b(e.pfx);
		U8 rex = (U8)(0x40 | (e.w ? 8 : 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (base >> 3));
		if(rex != 0x40 || e.rex)
			b(rex);
		if(e.map)
			b(0x0f);
		if(e.map && e.map != 0x0f)
			b(e.map);
		b(e.opc);
	}

	void Asm::rr(Enc e, U32 reg, U32 rm) {
		lead(e, reg, 0, rm);
		b((U8)(0xc0 | ((reg & 7) << 3) | (rm & 7)));
	}

	void Asm::rm(Enc e, U32 reg, const Mem& mem) {
		lead(e, reg, mem.index, mem.base);
		modrmMem(reg, mem);
	}

	// mod=00 carries no displacement, but base=101 means rip there, so rbp and r13 take a disp8.
	// rm=100 means a sib byte follows, so rsp and r12 bases need one even unindexed
	void Asm::modrmMem(U32 reg, const Mem& mem) {
		U32 mod = 2;
		if(mem.disp == 0 && (mem.base & 7) != 5)
			mod = 0;
		else if(mem.disp == (I32)(I8)mem.disp)
			mod = 1;
		B32 sib = mem.indexed || (mem.base & 7) == 4;
		b((U8)((mod << 6) | ((reg & 7) << 3) | (sib ? 4 : (mem.base & 7))));
		if(mem.indexed)
			b((U8)(((mem.scaleLog2 & 3) << 6) | ((mem.index & 7) << 3) | (mem.base & 7)));
		else if(sib)
			b(0x24);
		if(mod == 1)
			b((U8)mem.disp);
		else if(mod == 2)
			d32((U32)mem.disp);
	}

	U32 Asm::rip(Enc e, U32 reg) {
		lead(e, reg, 0, 0);
		b((U8)(0x05 | ((reg & 7) << 3)));
		return reserve32();
	}

	void Asm::ripSym(Enc e, U32 reg, const String& sym) {
		relocs.push_back({rip(e, reg), sym, RelocKind::Pc32, -4});
	}

	void Asm::opReg(Enc e, U32 r) {
		e.opc = (U8)(e.opc + (r & 7));
		lead(e, 0, 0, r);
	}

	void Asm::rrImm(U8 op8, U8 op32, U32 reg, U32 rm, I32 imm) {
		B32 short8 = imm == (I32)(I8)imm;
		rr({0, 0, short8 ? op8 : op32, 1}, reg, rm);
		this->imm(imm, short8 ? 1 : 4);
	}

	void Asm::aluImm(U8 ext, Reg r, I32 imm) { rrImm(0x83, 0x81, ext, r, imm); }

	void Asm::movImm64(Reg r, U64 imm) {
		opReg({0, 0, 0xb8, 1}, r);
		d64(imm);
	}

	// mov, movsxd, movzx or movsx by width and sign
	void Asm::load(Reg dst, const Mem& mem, U32 width, B32 sign) {
		Enc e{0, 0, 0x8b, 1};
		if(width == 4) {
			e.opc = sign ? 0x63 : 0x8b;
			e.w = sign;
		} else if(width < 4) {
			e.map = 0x0f;
			e.opc = (U8)((sign ? 0xbe : 0xb6) + (width == 2));
		}
		rm(e, dst, mem);
	}

	void Asm::store(const Mem& mem, Reg src, U32 width) {
		U8 opc = width == 1 ? 0x88 : 0x89;
		B32 byteReg = width == 1 && src >= RSP && src <= RDI;
		rm({(U8)(width == 2 ? 0x66 : 0), 0, opc, width == 8, byteReg}, src, mem);
	}

	void Asm::storeImm(const Mem& mem, I64 imm, U32 width) {
		U8 opc = width == 1 ? 0xc6 : 0xc7;
		rm({(U8)(width == 2 ? 0x66 : 0), 0, opc, width == 8}, 0, mem);
		this->imm(imm, std::min(width, 4u));
	}

	// movss, movsd or movups by width
	U8 Asm::ssePrefixByte(U32 width) {
		if(width == 16)
			return 0;
		return width == 4 ? 0xf3 : 0xf2;
	}
	void Asm::loadXmm(U32 xmm, const Mem& mem, U32 width) {
		rm({ssePrefixByte(width), 0x0f, 0x10}, xmm, mem);
	}
	void Asm::storeXmm(U32 xmm, const Mem& mem, U32 width) {
		rm({ssePrefixByte(width), 0x0f, 0x11}, xmm, mem);
	}

	U32 Asm::reserve32() {
		U32 at = here();
		d32(0);
		return at;
	}

	U32 Asm::jmpRel32() {
		b(0xe9);
		return reserve32();
	}

	U32 Asm::jccRel32(U8 cc) {
		b(0x0f);
		b((U8)(0x80 + cc));
		return reserve32();
	}

	void Asm::patchRel32(U32 dispAt, U32 target) {
		U32 rel = (U32)((I32)target - (I32)(dispAt + 4));
		le::wr(&code[dispAt], rel, 4);
	}

	void Asm::callSym(const String& sym) {
		b(0xe8);
		relocs.push_back({reserve32(), sym, RelocKind::Plt32, -4});
	}
} // namespace rat
