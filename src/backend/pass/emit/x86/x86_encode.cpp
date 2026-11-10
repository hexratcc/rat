#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/object_file.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

#include <functional>

namespace rat {
	namespace detail {
		constexpr U32 kByvalInlineBytes = 64;
	} // namespace detail

	Reg X86EncodePass::toGp(PhysReg p) { return (Reg)(p - X86Target::kGpBase); }
	Reg X86EncodePass::gpOf(const MachineOperand& o) { return toGp(o.phys); }
	U32 X86EncodePass::xmmOf(const MachineOperand& o) { return o.phys - X86Target::kXmmBase; }
	U32 X86EncodePass::scaleOf(const MachineInstr& in) { return (U32)((in.imm2 >> 2) & 3); }

	void X86EncodePass::readGp(const MachineOperand& o, Reg r) {
		if(o.kind == MachineOperand::Kind::Imm)
			a->movRegImm64(r, (U64)o.imm);
		else if(o.isPhys()) {
			if(gpOf(o) != r)
				a->movRR(r, gpOf(o));
		} else if(o.kind == MachineOperand::Kind::FrameSlot)
			a->load64(r, RBP, o.slot);
	}

	void X86EncodePass::stash(Reg r) { a->storeMem(RBP, fl->ldScratch, r, 8); }
	void X86EncodePass::unstash(Reg r) { a->load64(r, RBP, fl->ldScratch); }

	void X86EncodePass::immToScratch(I64 v) {
		a->movRegImm64(R11, (U64)v);
		stash(R11);
	}

	// kind in imm, width and sign in imm2; the value address is in r11
	void X86EncodePass::vaLoadResult(const MachineInstr& in) {
		VaArgKind kind = (VaArgKind)in.imm;
		if(kind == VaArgKind::Aggregate)
			return a->movRR(gpOf(in.defs[0]), R11);
		U32 width = (U32)(in.imm2 & 0xffffffff);
		if(kind == VaArgKind::X87) {
			a->fldT(R11, 0);
			return fstpSlot(in.defs[0]);
		}
		if(kind == VaArgKind::Sse)
			return a->loadXmm(xmmOf(in.defs[0]), R11, 0, width);
		a->loadExt(gpOf(in.defs[0]), R11, 0, width, (in.imm2 >> 32) != 0);
	}

	void X86EncodePass::emitVaStart(const MachineInstr& in) {
		readGp(in.uses[0], R10);
		if(conv->vaList == X86VaList::CharPtr) {
			a->leaMem(R11, RBP, fl->overflowOff);
			return a->storeMem(R10, 0, R11, 8);
		}
		a->movRegImm64(R11, (U32)in.imm * 8);
		a->storeMem(R10, 0, R11, 4);
		a->movRegImm64(R11, conv->gpSaveBytes + (U32)in.imm2 * conv->sseSlotBytes);
		a->storeMem(R10, 4, R11, 4);
		a->leaMem(R11, RBP, fl->overflowOff);
		a->storeMem(R10, 8, R11, 8);
		a->leaMem(R11, RBP, fl->saveArea);
		a->storeMem(R10, 16, R11, 8);
	}

	void X86EncodePass::vaFetchOverflow(I32 step, U32 align) {
		a->load64(R11, R10, 8); // R11 = overflow_arg_area
		if(align > 8) {
			// round up to the argument alignment
			a->addRegImm32(R11, (I32)align - 1);
			a->aluImm(4, R11, -(I32)align); // and
		}
		stash(R11);									 // stash address
		a->addRegImm32(R11, step);	 // advance
		a->storeMem(R10, 8, R11, 8); // write back overflow_arg_area
		unstash(R11);								 // R11 = stashed address
	}

	void X86EncodePass::vaFetch(I32 offDisp, U32 limit, I32 regStep) {
		U32 toStack = vaNoRoom(offDisp, limit - (U32)regStep);
		stash(R11); // stash original offset
		a->addRegImm32(R11, regStep);
		a->storeMem(R10, offDisp, R11, 4); // write advanced offset
		unstash(R11);											 // R11 = original offset
		a->addRegMem(R11, R10, 16);				 // R11 += reg_save_area base
		U32 done = a->jmpRel32();
		a->patchRel32(toStack, a->here());
		vaFetchOverflow(8, 8);
		a->patchRel32(done, a->here());
	}

	// jumps to the overflow path when the class offset is past limit
	U32 X86EncodePass::vaNoRoom(I32 offDisp, U32 limit) {
		a->loadExt(R11, R10, offDisp, 4, false);
		a->cmpRegImm32(R11, (I32)limit);
		return a->jccRel32(CC_A);
	}

	// [rbp + disp] = the next saved register of the class, then advance its offset
	void X86EncodePass::vaCopyEightbyte(I32 offDisp, I32 regStep, I32 disp) {
		a->loadExt(R11, R10, offDisp, 4, false);
		a->addRegMem(R11, R10, 16);
		a->load64(R11, R11, 0);
		a->storeMem(RBP, disp, R11, 8);
		a->loadExt(R11, R10, offDisp, 4, false);
		a->addRegImm32(R11, regStep);
		a->storeMem(R10, offDisp, R11, 4);
	}

	// all eightbytes from the save area into the frame temp in use 1, or none of them
	void X86EncodePass::vaFetchAggregate(const MachineInstr& in) {
		VaAggregate agg = detail::unpackVaAggregate(in.imm2);
		I32 step = (I32)((agg.bytes + 7u) & ~7u);
		if(!agg.regs.count)
			return vaFetchOverflow(step, agg.align);
		U32 gp = agg.regs.countOf(AbiClass::Integer);
		U32 sse = agg.regs.countOf(AbiClass::Sse);
		List<U32> toStack;
		if(gp)
			toStack.push_back(vaNoRoom(0, conv->gpSaveBytes - 8 * gp));
		if(sse)
			toStack.push_back(vaNoRoom(4, conv->regSaveBytes - conv->sseSlotBytes * sse));
		I32 tmp = in.uses[1].slot;
		for(U32 k = 0; k < agg.regs.count; ++k) {
			if(agg.regs.cls[k] == AbiClass::Sse)
				vaCopyEightbyte(4, (I32)conv->sseSlotBytes, tmp + 8 * (I32)k);
			else
				vaCopyEightbyte(0, 8, tmp + 8 * (I32)k);
		}
		a->leaMem(R11, RBP, tmp);
		U32 done = a->jmpRel32();
		for(U32 jump : toStack)
			a->patchRel32(jump, a->here());
		vaFetchOverflow(step, agg.align);
		a->patchRel32(done, a->here());
	}

	void X86EncodePass::emitVaArg(const MachineInstr& in) {
		readGp(in.uses[0], R10);
		VaArgKind kind = (VaArgKind)in.imm;
		if(conv->vaList == X86VaList::CharPtr) {
			a->load64(R11, R10, 0);
			stash(R11);
			a->addRegImm32(R11, 8);
			a->storeMem(R10, 0, R11, 8);
			unstash(R11);
			B32 byRef = kind == VaArgKind::X87 ||
									(kind == VaArgKind::Aggregate && !detail::unpackVaAggregate(in.imm2).regs.count);
			if(byRef)
				a->load64(R11, R11, 0); // slot holds a pointer to the value
		} else if(kind == VaArgKind::X87)
			vaFetchOverflow(16, 16);
		else if(kind == VaArgKind::Aggregate)
			vaFetchAggregate(in);
		else if(kind == VaArgKind::Sse)
			vaFetch(4, conv->regSaveBytes, (I32)conv->sseSlotBytes);
		else
			vaFetch(0, conv->gpSaveBytes, 8);
		vaLoadResult(in);
	}

	// a by-value aggregate into the outgoing area, a small one through the xmm scratch
	// a large one with rep movsq, rsi rdi and rcx wait in scratch registers
	void X86EncodePass::copyByval(const MachineOperand& src, U32 bytes, I32 off) {
		if(bytes <= detail::kByvalInlineBytes) {
			U32 chunk = conv->sseVolatileCount - 1;
			readGp(src, R10);
			for(U32 k = 0; k < bytes; k += 8) {
				a->loadXmm(chunk, R10, (I32)k, 8);
				a->storeXmm(chunk, RSP, off + (I32)k, 8);
			}
			return;
		}
		U32 keepRsi = conv->sseVolatileCount - 1;
		U32 keepRdi = conv->sseVolatileCount - 2;
		a->movdXmmGp(keepRsi, RSI, true);
		a->movdXmmGp(keepRdi, RDI, true);
		a->movRR(R10, RCX);
		readGp(src, RSI);
		a->leaMem(RDI, RSP, off);
		a->movRegImm32(RCX, (bytes + 7u) / 8u);
		a->repMovsq();
		a->movRR(RCX, R10);
		a->movGpXmm(RDI, keepRdi, true);
		a->movGpXmm(RSI, keepRsi, true);
	}

	void X86EncodePass::emitCall(const MachineInstr& in) {
		B32 indirect = in.imm2 != 0;
		U32 targetIdx = 0;
		for(U32 i = 0; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(!indirect && u.kind == MachineOperand::Kind::Sym)
				targetIdx = i;
			else if(indirect && u.isPhys() && u.phys == detail::gpPhys(R11))
				targetIdx = i;
		}

		U32 total = ((U32)in.imm + conv->shadowBytes + 15u) & ~15u;
		if(total)
			a->subRegImm32(RSP, (I32)total);
		I32 off = (I32)conv->shadowBytes;
		for(U32 i = targetIdx + 1; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(i + 1 < in.uses.size() && in.uses[i + 1].kind == MachineOperand::Kind::Imm) {
				U64 byval = (U64)in.uses[++i].imm; // stack offset in the high half, bytes in the low
				U32 bytes = (U32)byval;
				off = (I32)conv->shadowBytes + (I32)(byval >> 32);
				copyByval(u, bytes, off);
				off += (I32)((bytes + 7u) & ~7u);
				continue;
			}
			if(u.kind == MachineOperand::Kind::FrameSlot) {
				if(u.width == 16) { // by-value x87
					off = (off + 15) & ~15;
					fldSlot(u);
					a->fstpT(RSP, off);
					off += 16;
					continue;
				}
				a->load64(R10, RBP, u.slot);
				a->storeMem(RSP, off, R10, 8);
			} else if(X86Target::isXmm(u.phys)) {
				a->storeXmm(xmmOf(u), RSP, off, 8);
			} else {
				a->storeMem(RSP, off, gpOf(u), 8);
			}
			off += 8;
		}
		if(conv->dupSseArgsInGp) {
			// variadic callees read every register argument from the gp set
			for(U32 i = targetIdx + 1; i-- > 0;) {
				const MachineOperand& u = in.uses[i];
				if(!u.isPhys() || !X86Target::isXmm(u.phys) || xmmOf(u) >= conv->gpArgCount)
					continue;
				a->movGpXmm(conv->gpArgs[xmmOf(u)], xmmOf(u), true);
			}
		}

		if(indirect)
			a->callReg(R11);
		else
			a->callSym(in.uses[targetIdx].sym());
		if(total)
			a->addRegImm32(RSP, (I32)total);
	}

	void X86EncodePass::emitRet() {
		if(omitFrame)
			return a->ret();
		const List<PhysReg>& saved = fn->usedCalleeSaved;
		if(!saved.empty()) {
			// dynamic allocas move rsp, so re-point it at the save area first
			if(hasDynAlloca)
				a->leaMem(RSP, RBP, -(I32)(frameSize + 8u * (U32)saved.size()));
			for(U32 i = (U32)saved.size(); i-- > 0;)
				a->pop(toGp(saved[i]));
		}
		// the cfa falls back to rsp once leave drops the frame, code after ret is still framed
		a->leave();
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RSP), 8);
		a->ret();
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RBP), detail::kCfaFromRbp);
	}

	void X86EncodePass::emitSwitchJump(const MachineInstr& in) {
		// lea r10,[rip+table]; movsxd r11,[r10+sel*4]; add r11,r10; jmp r11
		// entries are offsets from the table base, so it is PIC, no relocations
		Reg sel = gpOf(in.uses[0]);
		// a spilled selector reloads into scratch r10/r11; route around it
		Reg base = sel == R10 ? R11 : R10;
		Reg entry = base == R10 ? R11 : R10; // may alias sel: read before written
		PendingTable t;
		t.leaDispAt = a->leaRipDisp(base);
		for(U32 i = 1; i < (U32)in.uses.size(); ++i)
			t.targets.push_back(in.uses[i].block);
		a->movsxdSib4(entry, base, sel);
		a->addRR(entry, base);
		a->jmpReg(entry);
		tables.push_back(std::move(t));
	}

	void X86EncodePass::emitBr(const MachineInstr& in, I32 fallthrough) {
		U8 cc;
		I32 thenB, elseB;
		if(in.imm2) { // fused: flags already set by the preceding cmp; cc in imm
			cc = (U8)in.imm;
			thenB = in.uses[0].block;
			elseB = in.uses[1].block;
		} else {
			Reg p = gpOf(in.uses[0]);
			a->testRR(p, p);
			cc = CC_NE;
			thenB = in.uses[1].block;
			elseB = in.uses[2].block;
		}
		if(thenB == fallthrough) // invert so the taken edge is the non-adjacent one
			return fixes.push_back({a->jccRel32((U8)(cc ^ 1)), elseB});
		fixes.push_back({a->jccRel32(cc), thenB});
		if(elseB != fallthrough)
			fixes.push_back({a->jmpRel32(), elseB});
	}

	void X86EncodePass::emitInst(const MachineInstr& in, I32 fallthrough) {
		X86Op op = (X86Op)in.op;
		switch(op) {
		case X86Op::Copy:
			return emitCopy(in);
		case X86Op::LoadImm:
			return emitLoadImm(in);
		case X86Op::LoadSym:
			return a->leaRipSym(gpOf(in.defs[0]), in.uses[0].sym(), 0);
		case X86Op::FrameAddr:
			return a->leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm);
		case X86Op::RetAddr:
			return a->load64(gpOf(in.defs[0]), RBP, 8);
		case X86Op::StackAlloc:
			return emitStackAlloc(in);
		case X86Op::StackSave:
			return a->movRR(gpOf(in.defs[0]), RSP);
		case X86Op::StackRestore:
			readGp(in.uses[0], R10);
			return a->movRR(RSP, R10);
		case X86Op::SetJmp:
			return emitSetJmp(in);
		case X86Op::LongJmp:
			return emitLongJmp(in);
		case X86Op::Lea: {
			U32 sc = (U32)(in.imm2 & 3);
			return a->leaSib(gpOf(in.defs[0]), gpOf(in.uses[0]), gpOf(in.uses[1]), sc, (I32)in.imm);
		}
		case X86Op::Load:
			return emitLoad(in);
		case X86Op::Store:
			return emitStore(in);
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor: {
			static const U8 kAlu[] = {
					detail::kAluAdd, detail::kAluSub, 0, detail::kAluAnd, detail::kAluOr, detail::kAluXor};
			static_assert((U32)X86Op::Xor - (U32)X86Op::Add + 1 == 6, "kAlu must cover Add..Xor");
			return emitAlu(in, kAlu[(U32)op - (U32)X86Op::Add]);
		}
		case X86Op::Mul:
			if(in.uses[1].kind == MachineOperand::Kind::Imm)
				return a->imulRRI(gpOf(in.defs[0]), gpOf(in.uses[0]), (I32)in.uses[1].imm);
			return a->imulRR(gpOf(in.defs[0]), gpOf(in.uses[1]));
		case X86Op::Neg:
			return a->negReg(gpOf(in.defs[0]));
		case X86Op::Not:
			return a->notReg(gpOf(in.defs[0]));
		case X86Op::Shl:
		case X86Op::AShr:
		case X86Op::LShr: {
			static const U8 kShift[] = {4, 7, 5}; // group-2 /ext for shl, sar, shr
			return emitShift(in, kShift[(U32)op - (U32)X86Op::Shl]);
		}
		case X86Op::Rotl:
		case X86Op::Rotr:
			return a->rotImm(op == X86Op::Rotr, gpOf(in.defs[0]), (U8)in.uses[1].imm, in.imm == 64);
		case X86Op::SDiv:
		case X86Op::SRem:
			return emitDiv(in, true);
		case X86Op::UDiv:
		case X86Op::URem:
			return emitDiv(in, false);
		case X86Op::BitScanF:
		case X86Op::BitScanR:
			return a->bitScan(op == X86Op::BitScanR, gpOf(in.defs[0]), gpOf(in.uses[0]), in.imm == 64);
		case X86Op::Cmp:
			return emitCmp(in);
		case X86Op::SetCC:
			return setccExt((U8)in.imm, gpOf(in.defs[0]));
		case X86Op::CMov:
			return a->cmovcc((U8)in.imm, gpOf(in.defs[0]), gpOf(in.uses[1]));
		case X86Op::MaskBits:
			return emitExtBits(in, false);
		case X86Op::SignExtBits:
			return emitExtBits(in, true);
		case X86Op::Bswap:
			return a->bswap(gpOf(in.defs[0]), in.imm == 64);
		case X86Op::Ud2:
			return a->ud2();
		case X86Op::Prefetch:
			return a->prefetch((U8)in.imm2, gpOf(in.uses[0]), (I32)in.imm);
		case X86Op::FLoad:
			return emitFLoad(in);
		case X86Op::FStore:
			return emitFStore(in);
		case X86Op::FAdd:
		case X86Op::FSub:
		case X86Op::FMul:
		case X86Op::FDiv: {
			U8 sseOp = detail::kSseOp[(U32)op - (U32)X86Op::FAdd];
			return a->sseArith(sseOp, (U32)in.imm, xmmOf(in.defs[0]), xmmOf(in.uses[1]));
		}
		case X86Op::FNeg:
			return emitFSign(in, false);
		case X86Op::FSqrt:
			return a->sseArith(0x51, (U32)in.imm, xmmOf(in.defs[0]), xmmOf(in.uses[0]));
		case X86Op::FAbs:
			return emitFSign(in, true);
		case X86Op::FCmp:
			emitFCmpFlags(in);
			return setccExt((U8)in.imm, gpOf(in.defs[0]));
		case X86Op::FCmpFlags:
			return emitFCmpFlags(in);
		case X86Op::Cvt:
			return emitCvt(in);
		case X86Op::VArith: {
			U64 imm = (U64)in.imm;
			U32 d = xmmOf(in.defs[0]), s = xmmOf(in.uses[1]);
			a->ssePacked((U8)(imm >> 8), (U8)imm, d, s, (U8)(imm >> 16));
			if(imm & detail::kVecImm8)
				a->b((U8)in.imm2);
			return;
		}
		case X86Op::VUnary: {
			U64 imm = (U64)in.imm;
			U32 d = xmmOf(in.defs[0]), s = xmmOf(in.uses[0]);
			return a->ssePacked((U8)(imm >> 8), (U8)imm, d, s, (U8)(imm >> 16));
		}
		case X86Op::VShiftImm:
			return a->sseShiftImm((U32)in.imm, (U8)(in.imm2 >> 8), xmmOf(in.defs[0]), (U8)in.imm2);
		case X86Op::VSplat:
			return emitVSplat(in);
		case X86Op::VExtract:
			return emitVExtract(in);
		case X86Op::VPack:
			return a->loadXmm(xmmOf(in.defs[0]), RBP, fl->vecScratch, 16);
		case X86Op::VPackLane:
			return emitVPackLane(in);
		case X86Op::VPackReg:
			return a->movdXmmGp(xmmOf(in.defs[0]), gpOf(in.uses[0]), (U32)in.imm == 8);
		case X86Op::VInsertReg:
			return a->pinsr(xmmOf(in.defs[0]), gpOf(in.uses[1]), (U8)in.imm2, (U32)in.imm == 8);
		case X86Op::VShuf:
			return a->pshufd(xmmOf(in.defs[0]), xmmOf(in.uses[0]), (U8)in.imm);
		case X86Op::X87LoadMem:
			if(in.imm == -1)
				return fldSlot(in.uses[0]);
			a->fldT(gpOf(in.uses[0]), 0);
			return fstpSlot(in.defs[0]);
		case X86Op::X87StoreMem:
			return emitX87StoreMem(in);
		case X86Op::X87LoadImmD:
			immToScratch(in.uses[0].imm);
			a->fldL(RBP, fl->ldScratch);
			return fstpSlot(in.defs[0]);
		case X86Op::X87FromInt:
			readGp(in.uses[0], R11);
			stash(R11);
			a->fildQ(RBP, fl->ldScratch);
			return fstpSlot(in.defs[0]);
		case X86Op::X87ToInt:
			return emitX87ToInt(in);
		case X86Op::X87FromSse:
			return emitX87FromSse(in);
		case X86Op::X87ToSse:
			return emitX87ToSse(in);
		case X86Op::X87Add:
		case X86Op::X87Sub:
		case X86Op::X87Mul:
		case X86Op::X87Div:
			return emitX87Binary(in, (U32)op - (U32)X86Op::X87Add);
		case X86Op::X87Neg:
			fldSlot(in.uses[0]);
			a->fchs();
			return fstpSlot(in.defs[0]);
		case X86Op::X87Cmp:
			return emitX87Cmp(in);
		case X86Op::Call:
			return emitCall(in);
		case X86Op::Ret:
			return emitRet();
		case X86Op::Jmp:
			if(in.uses[0].block != fallthrough)
				fixes.push_back({a->jmpRel32(), in.uses[0].block});
			return;
		case X86Op::SwitchJump:
			return emitSwitchJump(in);
		case X86Op::Br:
			return emitBr(in, fallthrough);
		case X86Op::VaStart:
			return emitVaStart(in);
		case X86Op::VaArg:
			return emitVaArg(in);
		}
	}

	void X86EncodePass::prologue() {
		if(omitFrame)
			return; // frameless leaf: no frame pointer
		a->push(RBP);
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RSP), detail::kCfaFromRbp);
		markCfi(CfiOp::Kind::Offset, detail::gpPhys(RBP), -detail::kCfaFromRbp);
		a->movRR(RBP, RSP);
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RBP), detail::kCfaFromRbp);
		if(conv->probeStack && frameSize > 4096) {
			// touch each page in order so the guard page is grown correctly
			U32 remaining = frameSize;
			while(remaining >= 4096) {
				a->subRegImm32(RSP, 4096);
				a->probeRsp();
				remaining -= 4096;
			}
			if(remaining)
				a->subRegImm32(RSP, (I32)remaining);
		} else if(frameSize) {
			a->subRegImm32(RSP, (I32)frameSize);
		}
		// callee saves below the frame slots, via push/pop
		I32 saveAt = -detail::kCfaFromRbp - (I32)frameSize;
		for(PhysReg r : fn->usedCalleeSaved) {
			a->push(toGp(r));
			saveAt -= 8;
			markCfi(CfiOp::Kind::Offset, r, saveAt);
		}
		if(!fl->variadic)
			return;
		// spill the positional register arguments into their home slots
		B32 win64 = conv->vaList == X86VaList::CharPtr;
		I32 gpBase = win64 ? conv->homeOff : fl->saveArea;
		for(U32 i = 0; i < conv->gpArgCount; ++i)
			a->storeMem(RBP, gpBase + (I32)(i * 8), conv->gpArgs[i], 8);
		if(win64)
			return;
		a->testRR(RAX, RAX);
		U32 skip = a->jccRel32(CC_E);
		for(U32 i = 0; i < conv->sseArgCount; ++i)
			a->storeXmm(i, RBP, fl->saveArea + (I32)conv->gpSaveBytes + (I32)(i * conv->sseSlotBytes), 8);
		a->patchRel32(skip, a->here());
	}

	void X86EncodePass::markCfi(CfiOp::Kind kind, PhysReg reg, I32 value) {
		if(encodedFunc)
			encodedFunc->cfi.push_back({a->here(), kind, detail::kDwarfRegs[reg], value});
	}

	// a row per source line change. the decl row at 0 always gets a second row after it, where gdb puts
	// the breakpoint past the prologue
	void X86EncodePass::markLine(U32 loc) {
		if(!encodedFunc || encodedFunc->lines.empty() || !loc)
			return;
		SrcLoc src = encodedFunc->fn->getDebug()->locs[loc].src;
		if(!src.line)
			return;
		LineRow& last = encodedFunc->lines.back();
		B32 body = encodedFunc->lines.size() > 1;
		if(body && last.loc.line == src.line && last.loc.file == src.file && last.loc.col == src.col)
			return;
		if(body && last.at == a->here())
			last.loc = src;
		else
			encodedFunc->lines.push_back({a->here(), src});
	}

	// [start, end) belongs to the instr's scope and its ancestors, located instrs carry the scope on
	void X86EncodePass::markScope(U32 loc, U32 start, U32 end) {
		if(!encodedFunc || encodedFunc->scopes.empty() || start == end)
			return;
		const DebugBody& body = *encodedFunc->fn->getDebug();
		if(loc)
			scope = body.locs[loc].scope;
		for(U32 s = scope; s != 0; s = body.scopes[s].parent) {
			List<PcRange>& r = encodedFunc->scopes[s];
			if(!r.empty() && r.back().end == start)
				r.back().end = end;
			else
				r.push_back({start, end});
		}
	}

	void X86EncodePass::recordVars(const Function& f) {
		if(!f.getDebug())
			return;
		for(const DebugVarInst& v : f.getDebug()->vars) {
			I32 off = v.slot ? fl->allocOff[v.slot->getId()] : kNoSlot;
			encodedFunc->varFrameOff.push_back(off == kNoSlot ? kNoFrameSlot : off - detail::kCfaFromRbp);
		}
	}

	U32 X86EncodePass::blockIdBound(const MachineFunc& f) {
		I32 maxId = -1;
		for(const MachineBlock& blk : f.blocks)
			if(blk.id > maxId)
				maxId = blk.id;
		return (U32)(maxId + 1);
	}

	B32 X86EncodePass::touchesFrame(const MachineOperand& o) {
		return o.kind == MachineOperand::Kind::FrameSlot || (o.isPhys() && gpOf(o) == RBP);
	}

	B32 X86EncodePass::needsFrame(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		if(in.isCall || op == X86Op::FrameAddr)
			return true;
		if(op == X86Op::VaStart || op == X86Op::VaArg || op == X86Op::RetAddr || op == X86Op::SetJmp ||
			 op == X86Op::LongJmp || (op >= X86Op::X87LoadMem && op <= X86Op::X87Cmp))
			return true; // reading [rbp+8] or jumping across frames needs rbp to survive
		if(op == X86Op::FLoad && !in.uses.empty() && in.uses[0].kind == MachineOperand::Kind::Imm)
			return true; // materializes through the rbp scratch slot
		for(const MachineOperand& o : in.defs)
			if(touchesFrame(o))
				return true;
		for(const MachineOperand& o : in.uses)
			if(touchesFrame(o))
				return true;
		return false;
	}

	void X86EncodePass::encodeFunction(const MachineFunc& f, Asm& asm_) {
		fn = &f;
		fl = static_cast<const X86FrameLayout*>(f.aux.get());
		a = &asm_;
		fixes.clear();
		tables.clear();

		// frame slots in [rbp-frameSize, rbp); saves pushed below, total 16-aligned
		U32 saveBytes = 8u * (U32)f.usedCalleeSaved.size();
		frameSize = ((f.frameBytes + saveBytes + 15u) & ~15u) - saveBytes;

		// frameless when nothing touches rbp/rsp: no slots, saves, calls, variadic,
		// or frame-addressing instruction
		hasDynAlloca = false;
		B32 framey = f.frameBytes != 0 || saveBytes != 0 || fl->variadic;
		U32 instCount = 0;
		for(const MachineBlock& blk : f.blocks)
			for(const MachineInstr& in : blk.insts) {
				X86Op op = (X86Op)in.op;
				if(op == X86Op::StackAlloc || op == X86Op::StackSave || op == X86Op::StackRestore)
					hasDynAlloca = true;
				if(!framey && needsFrame(in))
					framey = true;
				++instCount;
			}
		omitFrame = !framey && !hasDynAlloca;
		a->code.reserve((U64)instCount * 16u + 64u); // cheap upper estimate

		blockOffset.assign(blockIdBound(f), 0);
		varsIn.clear();
		// like gcc's max-vartrack-size, huge functions keep their vars without locations
		const DebugBody* body = encodedFunc ? encodedFunc->fn->getDebug() : nullptr;
		U64 trackCost = body ? (U64)instCount * body->vars.size() : 0;
		if(body && f.dbg.size() > 1 && trackCost <= kMaxVarTrackCost)
			solveVars(f);
		B32 trackVars = !varsIn.empty();
		prologue();
		for(U32 bi = 0; bi < f.blocks.size(); ++bi) {
			const MachineBlock& blk = f.blocks[bi];
			if(blk.id < 0)
				continue;
			if(blk.align)
				a->alignNop(blk.align);
			blockOffset[blk.id] = a->here();
			I32 fallthrough = (bi + 1 < f.blocks.size()) ? (I32)f.blocks[bi + 1].id : -1;
			if(trackVars)
				beginVars(bi);
			for(const MachineInstr& in : blk.insts) {
				U32 at = a->here();
				markLine(in.loc);
				if(trackVars)
					applyRecords(liveVars, in.dbg, at);
				emitInst(in, fallthrough);
				markScope(in.loc, at, a->here());
				// a write ends its vars one byte in, so the instr start still shows them but no later pc does
				if(trackVars)
					clobber(liveVars, in, std::min(at + 1, a->here()));
			}
			if(trackVars)
				endVars(blk);
		}
		// jump tables: 4-byte offsets from the table base, emitted after the code
		for(const PendingTable& t : tables) {
			U32 base = a->here();
			a->patchRel32(t.leaDispAt, base);
			for(I32 tb : t.targets)
				a->d32(blockOffset[tb] - base);
		}
		for(const JumpFix& jf : fixes)
			a->patchRel32(jf.dispAt, blockOffset[jf.targetBlock]);
	}

	void X86EncodePass::emitGlobal(ObjectFile& obj, const Global* g, U32 ptrBytes) {
		const List<U8>& init = g->getInit();
		U32 size = g->getType()->byteSize(ptrBytes);
		if(size == 0)
			size = (U32)init.size();
		if(size == 0)
			size = 1;

		B32 allZero =
				g->getRelocs().empty() && std::all_of(init.begin(), init.end(), std::logical_not<U8>());
		ObjectFile::Section sec = ObjectFile::Data;
		if(allZero)
			sec = ObjectFile::Bss;
		else if(g->isConstant())
			sec = ObjectFile::Rodata;
		obj.align(sec, std::max(g->getAlign(), 8u));

		U32 off;
		if(allZero) {
			off = obj.appendZero(sec, size);
		} else {
			List<U8> img(size, 0);
			std::copy_n(init.begin(), std::min((U32)init.size(), size), img.begin());
			off = obj.append(sec, img.data(), size);
		}
		obj.defineSymbol(g->getName(), sec, off, !g->isInternal(), false);

		for(const Reloc& r : g->getRelocs())
			obj.addReloc(sec, off + r.offset, r.symbol, RelocKind::Abs64, r.addend);
	}

	B32 X86EncodePass::run(Module&, const Function& f, MachineFunc& mf, const TargetInfo& target) {
		conv = &x86CallConv(target.getTriple().os);
		if(!obj)
			obj = createObjectFile(target.getTriple().os);
		encodedFunc = nullptr;
		if(objectFormatFor(target.getTriple().os) == ObjectFormat::Elf) {
			encodedFunc = &encoded.emplace_back();
			encodedFunc->fn = &f;
			if(f.getDebug()) {
				encodedFunc->lines.push_back({0, f.getDebug()->func->decl});
				encodedFunc->scopes.resize(f.getDebug()->scopes.size());
				encodedFunc->varRanges.resize(f.getDebug()->vars.size());
			}
			scope = 0;
		}
		code.clear();
		Asm a(code, relocs);
		encodeFunction(mf, a);
		// block alignment is relative to the function start
		U32 align = std::max(f.getAttrs().align, 16u);
		for(const MachineBlock& blk : mf.blocks)
			align = std::max(align, blk.align);
		obj->align(ObjectFile::Text, align);
		U32 off = obj->append(ObjectFile::Text, code.data(), (U32)code.size());
		if(encodedFunc) {
			encodedFunc->textOff = off;
			encodedFunc->size = (U32)code.size();
			recordVars(f);
		}
		placed.push_back({&f, off, (U32)relocs.size()});
		return false;
	}

	// after all globals, so the symbol order holds while lowering adds constant-pool globals
	void X86EncodePass::defineFunctions() {
		U32 r = 0;
		for(const auto& [f, off, relocEnd] : placed) {
			obj->defineSymbol(f->getName(), ObjectFile::Text, off, !f->getAttrs().isInternal(), true);
			for(; r < relocEnd; ++r) {
				const AsmReloc& x = relocs[r];
				obj->addReloc(ObjectFile::Text, off + x.offset, x.symbol, x.kind, x.addend);
			}
		}
	}

	void X86EncodePass::finish(Module& mod, const TargetInfo& target) {
		if(!obj)
			obj = createObjectFile(target.getTriple().os);
		for(const Global* g : mod.globals())
			if(!g->isAlias())
				emitGlobal(*obj, g, target.getPointerSizeInBytes());
		defineFunctions();

		if(objectFormatFor(target.getTriple().os) == ObjectFormat::Elf)
			writeDwarf(mod, encoded, *obj);

		// aliases label storage that some other symbol owns
		for(const Global* g : mod.globals()) {
			if(!g->isAlias())
				continue;
			B32 ok = obj->defineAlias(g->getName(), g->getAliasTarget(), !g->isInternal());
			assert(ok && "alias target is not defined in this module");
			(void)ok;
		}

		obj->write(*os);
		obj = nullptr;
		relocs.clear();
		placed.clear();
		encoded.clear();
	}
} // namespace rat
