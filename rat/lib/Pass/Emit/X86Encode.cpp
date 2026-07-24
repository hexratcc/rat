#include "Pass/Emit/X86Encode.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/MachineModule.h"
#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Target/ObjectFile.h"
#include "Target/Target.h"
#include "Target/X86Asm.h"

namespace rat {
	Reg X86EncodePass::toGp(PhysReg p) { return (Reg)(p - X86Target::kGpBase); }
	U32 X86EncodePass::toXmm(PhysReg p) { return p - X86Target::kXmmBase; }
	Reg X86EncodePass::gpOf(const MachineOperand& o) { return toGp(o.phys); }
	U32 X86EncodePass::xmmOf(const MachineOperand& o) { return toXmm(o.phys); }
	PhysReg X86EncodePass::gpReg11() { return X86Target::kGpBase + (PhysReg)R11; }

	void X86EncodePass::reset(const MachineFunc& f,
														const X86FrameLayout& layout,
														Asm& asm_,
														List<PhysReg> callee) {
		fn = &f;
		fl = &layout;
		a = &asm_;
		blockOffset.clear();
		fixes.clear();
		frameSize = 0;
		calleeSaved = std::move(callee);
		calleeBase = 0;
	}

	void X86EncodePass::readGp(const MachineOperand& o, Reg r) {
		if(o.kind == MachineOperand::Kind::Imm)
			a->movRegImm64(r, (U64)o.imm);
		else if(o.kind == MachineOperand::Kind::Phys) {
			if(gpOf(o) != r)
				a->movRR(r, gpOf(o));
		} else if(o.kind == MachineOperand::Kind::FrameSlot)
			a->load64(r, RBP, o.slot);
	}

	void X86EncodePass::emitVaStartWin64(const MachineInstr& in) {
		Reg ptr = gpOf(in.uses[0]);
		if(ptr != R10)
			a->movRR(R10, ptr);
		a->leaMem(R11, RBP, fl->overflowOff);
		a->storeMem(R10, 0, R11, 8);
	}

	void X86EncodePass::emitVaArgWin64(const MachineInstr& in) {
		Reg ptr = gpOf(in.uses[0]);
		if(ptr != R10)
			a->movRR(R10, ptr);
		VaArgKind kind = (VaArgKind)in.imm;
		U32 width = (U32)(in.imm2 & 0xffffffff);
		B32 sign = (in.imm2 >> 32) != 0;
		a->load64(R11, R10, 0);
		a->storeMem(RBP, fl->ldScratch, R11, 8);
		a->addRegImm32(R11, 8);
		a->storeMem(R10, 0, R11, 8);
		a->load64(R11, RBP, fl->ldScratch);
		if(kind == VaArgKind::X87) {
			a->load64(R11, R11, 0); // slot holds a pointer to the value
			a->fldT(R11, 0);
			fstpSlot(in.defs[0].slot);
			return;
		}
		if(kind == VaArgKind::Sse) {
			a->loadXmm(xmmOf(in.defs[0]), R11, 0, width);
			return;
		}
		a->loadExt(gpOf(in.defs[0]), R11, 0, width, sign);
	}

	void X86EncodePass::emitVaStart(const MachineInstr& in) {
		if(conv->vaList == X86VaList::CharPtr) {
			emitVaStartWin64(in);
			return;
		}
		Reg ptr = gpOf(in.uses[0]);
		U32 namedGp = (U32)in.imm, namedFp = (U32)in.imm2;
		if(ptr != R10)
			a->movRR(R10, ptr);
		a->movRegImm64(R11, namedGp * 8);
		a->storeMem(R10, 0, R11, 4);
		a->movRegImm64(R11, conv->gpSaveBytes + namedFp * conv->sseSlotBytes);
		a->storeMem(R10, 4, R11, 4);
		a->leaMem(R11, RBP, fl->overflowOff);
		a->storeMem(R10, 8, R11, 8);
		a->leaMem(R11, RBP, fl->saveArea);
		a->storeMem(R10, 16, R11, 8);
	}

	void X86EncodePass::vaFetchOverflow(I32 step) {
		a->load64(R11, R10, 8);									 // R11 = overflow_arg_area
		a->storeMem(RBP, fl->ldScratch, R11, 8); // stash address
		a->addRegImm32(R11, step);							 // advance
		a->storeMem(R10, 8, R11, 8);						 // write back overflow_arg_area
		a->load64(R11, RBP, fl->ldScratch);			 // R11 = stashed address
	}

	void X86EncodePass::vaFetch(I32 offDisp, U32 limit, I32 regStep) {
		a->loadExt(R11, R10, offDisp, 4, false); // R11 = cur offset
		a->cmpRegImm32(R11, (I32)limit);				 // offset vs limit
		U32 toStack = a->jccRel32(CC_AE);				 // offset >= limit -> overflow path
		a->storeMem(RBP, fl->ldScratch, R11, 8); // stash original offset
		a->addRegImm32(R11, regStep);
		a->storeMem(R10, offDisp, R11, 4);	// write advanced offset
		a->load64(R11, RBP, fl->ldScratch); // R11 = original offset
		a->addRegMem(R11, R10, 16);					// R11 += reg_save_area base
		U32 done = a->jmpRel32();
		a->patchRel32(toStack, a->here());
		vaFetchOverflow(8);
		a->patchRel32(done, a->here());
	}

	void X86EncodePass::emitVaArg(const MachineInstr& in) {
		if(conv->vaList == X86VaList::CharPtr) {
			emitVaArgWin64(in);
			return;
		}
		Reg ptr = gpOf(in.uses[0]);
		if(ptr != R10)
			a->movRR(R10, ptr);
		VaArgKind kind = (VaArgKind)in.imm;
		U32 width = (U32)(in.imm2 & 0xffffffff);
		B32 sign = (in.imm2 >> 32) != 0;
		if(kind == VaArgKind::X87) {
			vaFetchOverflow(16);
			a->fldT(R11, 0);
			fstpSlot(in.defs[0].slot);
			return;
		}
		if(kind == VaArgKind::Sse) {
			vaFetch(4, conv->regSaveBytes, (I32)conv->sseSlotBytes);
			a->loadXmm(xmmOf(in.defs[0]), R11, 0, width);
			return;
		}
		vaFetch(0, conv->gpSaveBytes, 8);
		a->loadExt(gpOf(in.defs[0]), R11, 0, width, sign);
	}

	void X86EncodePass::emitCall(const MachineInstr& in) {
		I32 stackBytes = (I32)in.imm;
		B32 indirect = in.imm2 != 0;

		U32 targetIdx = 0;
		for(U32 i = 0; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(!indirect && u.kind == MachineOperand::Kind::Sym)
				targetIdx = i;
			else if(indirect && u.kind == MachineOperand::Kind::Phys && u.phys == gpReg11())
				targetIdx = i;
		}

		U32 total = ((U32)stackBytes + conv->shadowBytes + 15u) & ~15u;
		if(total)
			a->subRegImm32(RSP, (I32)total);
		I32 off = (I32)conv->shadowBytes;
		for(U32 i = targetIdx + 1; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(u.kind == MachineOperand::Kind::FrameSlot) {
				if(u.width == 16) { // by-value x87
					fldSlot(u.slot);
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
				if(u.kind == MachineOperand::Kind::Phys && X86Target::isXmm(u.phys) &&
					 toXmm(u.phys) < conv->gpArgCount)
					a->movqGpXmm(conv->gpArgs[toXmm(u.phys)], toXmm(u.phys));
			}
		}

		if(indirect)
			a->callReg(R11);
		else
			a->callSym(in.uses[targetIdx].sym);
		if(total)
			a->addRegImm32(RSP, (I32)total);
	}

	void X86EncodePass::recordFix(U32 dispAt, I32 targetBlock) {
		fixes.push_back({dispAt, targetBlock});
	}

	void X86EncodePass::emitRet(const MachineInstr&) {
		for(U32 i = 0; i < calleeSaved.size(); ++i)
			a->load64(toGp(calleeSaved[i]), RBP, calleeBase - (I32)(8 * (i + 1)));
		a->leave();
		a->ret();
	}

	void X86EncodePass::emitJmp(const MachineInstr& in, I32 fallthrough) {
		I32 target = in.uses[0].block;
		if(target == fallthrough)
			return;
		recordFix(a->jmpRel32(), target);
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
		if(thenB == fallthrough) { // invert so the taken edge is the non-adjacent one
			recordFix(a->jccRel32((U8)(cc ^ 1)), elseB);
			return;
		}
		recordFix(a->jccRel32(cc), thenB);
		if(elseB != fallthrough)
			recordFix(a->jmpRel32(), elseB);
	}

	void X86EncodePass::emitInst(const MachineInstr& in, I32 fallthrough) {
		switch((X86Op)in.op) {
		case X86Op::Copy:
			emitCopy(in);
			return;
		case X86Op::LoadImm:
			emitLoadImm(in);
			return;
		case X86Op::LoadSym:
			emitLoadSym(in);
			return;
		case X86Op::FrameAddr:
			emitFrameAddr(in);
			return;
		case X86Op::Lea:
			emitLea(in);
			return;
		case X86Op::Load:
			emitLoad(in);
			return;
		case X86Op::Store:
			emitStore(in);
			return;
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor: {
			static const U8 kAlu[] = {
					detail::kAluAdd, detail::kAluSub, 0, detail::kAluAnd, detail::kAluOr, detail::kAluXor};
			static_assert((U32)X86Op::Xor - (U32)X86Op::Add + 1 == 6, "kAlu must cover Add..Xor");
			emitAlu(in, kAlu[(U32)in.op - (U32)X86Op::Add]);
			return;
		}
		case X86Op::Mul:
			emitMul(in);
			return;
		case X86Op::Neg:
		case X86Op::Not:
			emitNegNot(in, (X86Op)in.op == X86Op::Neg);
			return;
		case X86Op::Shl:
			emitShift(in, 4);
			return;
		case X86Op::AShr:
			emitShift(in, 7);
			return;
		case X86Op::LShr:
			emitShift(in, 5);
			return;
		case X86Op::SDiv:
		case X86Op::SRem:
			emitDiv(in, true);
			return;
		case X86Op::UDiv:
		case X86Op::URem:
			emitDiv(in, false);
			return;
		case X86Op::Cmp:
			emitCmp(in);
			return;
		case X86Op::SetCC:
			emitSetCC(in);
			return;
		case X86Op::MaskBits:
			emitMaskBits(in);
			return;
		case X86Op::SignExtBits:
			emitSignExtBits(in);
			return;
		case X86Op::FLoad:
			emitFLoad(in);
			return;
		case X86Op::FStore:
			emitFStore(in);
			return;
		case X86Op::FAdd:
		case X86Op::FSub:
		case X86Op::FMul:
		case X86Op::FDiv:
			emitFArith(in, detail::kSseOp[(U32)in.op - (U32)X86Op::FAdd]);
			return;
		case X86Op::FNeg:
			emitFNeg(in);
			return;
		case X86Op::FSqrt:
			emitFSqrt(in);
			return;
		case X86Op::FAbs:
			emitFAbs(in);
			return;
		case X86Op::FCmp:
			emitFCmp(in);
			return;
		case X86Op::Cvt:
			emitCvt(in);
			return;
		case X86Op::X87LoadMem:
			emitX87LoadMem(in);
			return;
		case X86Op::X87StoreMem:
			emitX87StoreMem(in);
			return;
		case X86Op::X87LoadImmD:
			emitX87LoadImmD(in);
			return;
		case X86Op::X87FromInt:
			emitX87FromInt(in);
			return;
		case X86Op::X87ToInt:
			emitX87ToInt(in);
			return;
		case X86Op::X87FromSse:
			emitX87FromSse(in);
			return;
		case X86Op::X87ToSse:
			emitX87ToSse(in);
			return;
		case X86Op::X87Add:
		case X86Op::X87Sub:
		case X86Op::X87Mul:
		case X86Op::X87Div:
			emitX87Binary(in, (U32)in.op - (U32)X86Op::X87Add);
			return;
		case X86Op::X87Neg:
			emitX87Neg(in);
			return;
		case X86Op::X87Cmp:
			emitX87Cmp(in);
			return;
		case X86Op::Call:
			emitCall(in);
			return;
		case X86Op::Ret:
			emitRet(in);
			return;
		case X86Op::Jmp:
			emitJmp(in, fallthrough);
			return;
		case X86Op::Br:
			emitBr(in, fallthrough);
			return;
		case X86Op::VaStart:
			emitVaStart(in);
			return;
		case X86Op::VaArg:
			emitVaArg(in);
			return;
		}
	}

	void X86EncodePass::prologue() {
		a->push(RBP);
		a->movRR(RBP, RSP);
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
		for(U32 i = 0; i < calleeSaved.size(); ++i)
			a->storeMem(RBP, calleeBase - (I32)(8 * (i + 1)), toGp(calleeSaved[i]), 8);
		if(!fl->variadic)
			return;
		if(conv->vaList == X86VaList::CharPtr) {
			// spill the positional register arguments into their home slots
			for(U32 i = 0; i < conv->gpArgCount; ++i)
				a->storeMem(RBP, conv->homeOff + (I32)(i * 8), conv->gpArgs[i], 8);
			return;
		}
		for(U32 i = 0; i < conv->gpArgCount; ++i)
			a->storeMem(RBP, fl->saveArea + (I32)(i * 8), conv->gpArgs[i], 8);
		a->testRR(RAX, RAX);
		U32 skip = a->jccRel32(CC_E);
		for(U32 i = 0; i < conv->sseArgCount; ++i)
			a->storeXmm(i, RBP, fl->saveArea + (I32)conv->gpSaveBytes + (I32)(i * conv->sseSlotBytes), 8);
		a->patchRel32(skip, a->here());
	}

	void X86EncodePass::encodeFunction() {
		calleeBase = -(I32)fn->frameBytes;
		frameSize = (fn->frameBytes + 8u * (U32)calleeSaved.size() + 15u) & ~15u;
		blockOffset.assign(fn->blocks.size(), 0);
		prologue();
		for(U32 bi = 0; bi < fn->blocks.size(); ++bi) {
			const MachineBlock& blk = fn->blocks[bi];
			if(blk.id < 0)
				continue;
			blockOffset[blk.id] = a->here();
			I32 fallthrough = (bi + 1 < fn->blocks.size()) ? (I32)fn->blocks[bi + 1].id : -1;
			for(const MachineInstr& in : blk.insts)
				emitInst(in, fallthrough);
		}
		for(const JumpFix& f : fixes)
			a->patchRel32(f.dispAt, blockOffset[f.targetBlock]);
	}

	void X86EncodePass::emitGlobal(ObjectFile& obj, const Global* g, U32 ptrBytes) {
		const List<U8>& init = g->getInit();
		U32 size = g->getType()->byteSize(ptrBytes);
		if(size == 0)
			size = (U32)init.size();
		if(size == 0)
			size = 1;

		B32 allZero = g->getRelocs().empty() &&
									std::all_of(init.begin(), init.end(), [](U8 v) { return v == 0; });
		ObjectFile::Section sec =
				allZero ? ObjectFile::Bss : (g->isConstant() ? ObjectFile::Rodata : ObjectFile::Data);
		obj.align(sec, 8);

		U32 off;
		if(allZero) {
			off = obj.appendZero(sec, size);
		} else {
			List<U8> img(size, 0);
			std::copy_n(init.begin(), init.size() < size ? init.size() : size, img.begin());
			off = obj.append(sec, img.data(), size);
		}
		obj.defineSymbol(g->getName(), sec, off, !g->isInternal(), false);

		for(const Reloc& r : g->getRelocs())
			obj.addReloc(sec, off + r.offset, r.symbol, RelocKind::Abs64, r.addend);
	}

	B32 X86EncodePass::run(Module& mod, MachineModule& mm, const TargetInfo& target) {
		conv = &x86CallConv(target.getTriple().os);
		UniquePtr<ObjectFile> obj = createObjectFile(target.getTriple().os);

		for(const Global* g : mod.globals())
			emitGlobal(*obj, g, target.getPointerSizeInBytes());

		for(const Function* fn : mod) {
			MachineFunc& mf = mm.get(fn);

			const X86FrameLayout& fl = *static_cast<const X86FrameLayout*>(mf.aux.get());

			List<U8> code;				 // emitted machine code bytes
			List<AsmReloc> relocs; // relocations into code
			Asm a(code, relocs);
			reset(mf, fl, a, mf.usedCalleeSaved);
			encodeFunction();

			obj->align(ObjectFile::Text, 16);
			U32 off = obj->append(ObjectFile::Text, code.data(), (U32)code.size());
			B32 global = fn->getLinkage() == Function::Linkage::External;
			obj->defineSymbol(fn->getName(), ObjectFile::Text, off, global, true);
			for(const AsmReloc& r : relocs)
				obj->addReloc(ObjectFile::Text, off + r.offset, r.symbol, r.kind, r.addend);
		}

		obj->write(*os);
		return false;
	}

	RegAllocHooks X86Target::regAllocHooks() const { return x86RegAllocHooks(); }
} // namespace rat
