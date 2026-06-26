#include "Pass/Emit/X86Emitter.h"

#include "X86Lower.h"

#include "CodeGen/MachineModule.h"
#include "CodeGen/RegAlloc.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "Target/Target.h"
#include "Target/X86Asm.h"
#include "Target/X86Elf.h"

namespace rat {
	namespace {
		constexpr U32 kGp = X86Target::kGpClass;
		constexpr U32 kFp = X86Target::kFpClass;

		Reg toGp(PhysReg p) { return (Reg)(p - X86Target::kGpBase); }
		U32 toXmm(PhysReg p) { return p - X86Target::kXmmBase; }

		using namespace sysv;

		constexpr U8 kSseOp[] = {0x58, 0x5c, 0x59, 0x5e};

		struct Encoder {
			struct JumpFix {
				U32 dispAt;			 // offset of the rel32 displacement in code
				I32 targetBlock; // block the jump targets
			};

			const MachineFunc& fn;
			const X86FrameLayout& fl;
			Asm& a;
			List<U32> blockOffset; // block id -> byte offset in code
			List<JumpFix> fixes;
			U32 frameSize = 0;
			List<PhysReg> calleeSaved; // callee-saved GP regs the allocator used
			I32 calleeBase = 0;				 // RBP offset of the first callee-save slot

			Encoder(const MachineFunc& f, const X86FrameLayout& layout, Asm& asm_, List<PhysReg> callee)
			: fn(f),
				fl(layout),
				a(asm_),
				calleeSaved(std::move(callee)) {}

			static Reg gpOf(const MachineOperand& o) { return toGp(o.phys); }
			static U32 xmmOf(const MachineOperand& o) { return toXmm(o.phys); }

			void readGp(const MachineOperand& o, Reg r) {
				if(o.kind == MachineOperand::Kind::Imm)
					a.movRegImm64(r, (U64)o.imm);
				else if(o.kind == MachineOperand::Kind::Phys) {
					if(gpOf(o) != r)
						a.movRR(r, gpOf(o));
				} else if(o.kind == MachineOperand::Kind::FrameSlot)
					a.load64(r, RBP, o.slot);
			}

			void emitCopy(const MachineInstr& in) {
				const MachineOperand& d = in.defs[0];
				const MachineOperand& s = in.uses[0];
				if(in.regClass == kFp) {
					U32 w = d.width;
					if(d.kind == MachineOperand::Kind::Phys && s.kind == MachineOperand::Kind::Phys) {
						if(xmmOf(d) != xmmOf(s))
							a.sseArith(0x10, w, xmmOf(d), xmmOf(s)); // movss/movsd reg,reg
					} else if(d.kind == MachineOperand::Kind::Phys &&
										s.kind == MachineOperand::Kind::FrameSlot)
						a.loadXmm(xmmOf(d), RBP, s.slot, w);
					else if(d.kind == MachineOperand::Kind::FrameSlot && s.kind == MachineOperand::Kind::Phys)
						a.storeXmm(xmmOf(s), RBP, d.slot, w);
					return;
				}
				if(d.kind == MachineOperand::Kind::Phys) {
					readGp(s, gpOf(d));
				} else if(d.kind == MachineOperand::Kind::FrameSlot) {
					if(s.kind == MachineOperand::Kind::Phys)
						a.storeMem(RBP, d.slot, gpOf(s), 8);
					else {
						readGp(s, R11);
						a.storeMem(RBP, d.slot, R11, 8);
					}
				}
			}

			void emitLoadImm(const MachineInstr& in) {
				a.movRegImm64(gpOf(in.defs[0]), (U64)in.uses[0].imm);
			}

			void emitLoadSym(const MachineInstr& in) { a.leaRipSym(gpOf(in.defs[0]), in.uses[0].sym, 0); }

			void emitFrameAddr(const MachineInstr& in) {
				if(in.imm == -1) {
					Reg d = gpOf(in.defs[0]);
					readGp(in.uses[0], R10);
					a.movRegImm64(R11, ~(U64)15);
					a.addRegImm32(R10, 15);
					a.andRR(R10, R11);
					a.subRR(RSP, R10);
					a.andRR(RSP, R11);
					a.movRR(d, RSP);
					return;
				}
				a.leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm);
			}

			void emitLoadFrame(const MachineInstr& in) { a.leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm); }

			void emitLoad(const MachineInstr& in) {
				const MachineOperand& d = in.defs[0];
				const MachineOperand& addr = in.uses[0];
				U32 w = d.width;
				if(addr.kind == MachineOperand::Kind::FrameSlot) {
					a.load64(gpOf(d), RBP, addr.slot);
					return;
				}
				Reg base = gpOf(addr);
				a.loadExt(gpOf(d), base, (I32)in.imm, w, in.imm2 != 0);
			}

			void emitStore(const MachineInstr& in) {
				const MachineOperand& a0 = in.uses[0];
				const MachineOperand& src = in.uses[1];
				if(a0.kind == MachineOperand::Kind::FrameSlot) {
					a.storeMem(RBP, a0.slot, gpOf(src), 8);
					return;
				}
				a.storeMem(gpOf(a0), (I32)in.imm, gpOf(src), src.width);
			}

			void emitFLoad(const MachineInstr& in) {
				const MachineOperand& d = in.defs[0];
				const MachineOperand& addr = in.uses[0];
				U32 w = d.width;
				if(addr.kind == MachineOperand::Kind::Imm) {
					a.movRegImm64(R11, (U64)addr.imm);
					a.storeMem(RBP, fl.ldScratch, R11, 8);
					a.loadXmm(xmmOf(d), RBP, fl.ldScratch, w);
					return;
				}
				if(addr.kind == MachineOperand::Kind::FrameSlot) {
					a.loadXmm(xmmOf(d), RBP, addr.slot, w);
					return;
				}
				a.loadXmm(xmmOf(d), gpOf(addr), (I32)in.imm, w);
			}

			void emitFStore(const MachineInstr& in) {
				const MachineOperand& a0 = in.uses[0];
				const MachineOperand& src = in.uses[1];
				if(a0.kind == MachineOperand::Kind::FrameSlot) {
					a.storeXmm(xmmOf(src), RBP, a0.slot, src.width);
					return;
				}
				a.storeXmm(xmmOf(src), gpOf(a0), (I32)in.imm, src.width);
			}

			void emitAlu(const MachineInstr& in, U8 aluOp) {
				Reg d = gpOf(in.defs[0]);
				Reg s = gpOf(in.uses[1]);
				a.aluRR(aluOp, d, s);
			}

			void emitMul(const MachineInstr& in) { a.imulRR(gpOf(in.defs[0]), gpOf(in.uses[1])); }

			void emitNegNot(const MachineInstr& in, B32 neg) {
				Reg d = gpOf(in.defs[0]);
				if(neg)
					a.negReg(d);
				else
					a.notReg(d);
			}

			void emitShift(const MachineInstr& in, U8 ext) {
				a.shiftCL(ext, gpOf(in.defs[0]));
			}

			void emitDiv(const MachineInstr& in, B32 isSigned) {
				B32 wide = (U32)in.imm > 32;
				if(isSigned) {
					a.cqoW(wide);
					a.idivRegW(RCX, wide);
				} else {
					a.xorSelf(RDX);
					a.divRegW(RCX, wide);
				}
				if(!wide) {
					a.movsxd32(RAX, RAX);
					a.movsxd32(RDX, RDX);
				}
			}

			void emitMaskBits(const MachineInstr& in) {
				U32 bits = (U32)in.imm;
				if(bits == 0 || bits >= 64)
					return;
				Reg d = gpOf(in.defs[0]);
				a.movRegImm64(R11, ((U64)1 << bits) - 1);
				a.andRR(d, R11);
			}

			void emitSignExtBits(const MachineInstr& in) {
				U32 bits = (U32)in.imm;
				if(bits == 0 || bits >= 64)
					return;
				Reg d = gpOf(in.defs[0]);
				if(bits == 32) {
					a.movsxd32(d, d);
					return;
				}
				U8 sh = (U8)(64 - bits);
				a.shiftImm(4, d, sh); // shl
				a.shiftImm(7, d, sh); // sar
			}

			void emitCmp(const MachineInstr& in) { a.cmpRR(gpOf(in.uses[0]), gpOf(in.uses[1])); }

			void emitSetCC(const MachineInstr& in) {
				Reg d = gpOf(in.defs[0]);
				a.setcc((U8)in.imm, d);
				a.movzxByte(d, d);
			}

			void emitFArith(const MachineInstr& in, U8 op) {
				U32 w = (U32)in.imm;
				a.sseArith(op, w, xmmOf(in.defs[0]), xmmOf(in.uses[1]));
			}

			void emitFNeg(const MachineInstr& in) {
				U32 w = (U32)in.imm;
				U32 d = xmmOf(in.defs[0]);
				U32 s = xmmOf(in.uses[0]);
				a.pxor(15, 15);
				a.sseArith(0x5c, w, 15, s == d ? d : s);
				if(d != 15)
					a.sseArith(0x10, w, d, 15);
			}

			void emitFCmp(const MachineInstr& in) {
				U32 w = in.uses[0].width;
				U32 lhs = xmmOf(in.uses[0]);
				U32 rhs = xmmOf(in.uses[1]);
				if(in.imm2)
					a.ucomis(w, rhs, lhs);
				else
					a.ucomis(w, lhs, rhs);
				Reg d = gpOf(in.defs[0]);
				a.setcc((U8)in.imm, d);
				a.movzxByte(d, d);
			}

			void emitCvt(const MachineInstr& in) {
				U8 pfx = (U8)((in.imm >> 16) & 0xff);
				U8 opc = (U8)((in.imm >> 8) & 0xff);
				B32 w = (in.imm & 1) != 0;
				const MachineOperand& d = in.defs[0];
				const MachineOperand& s = in.uses[0];
				U32 dst = (in.regClass == kGp) ? (U32)gpOf(d) : xmmOf(d);
				U32 srcReg;
				if(in.regClass == kGp)
					srcReg = xmmOf(s); // FPToSI/UI: xmm -> gp
				else if(X86Target::isXmm(s.phys))
					srcReg = xmmOf(s); // fp -> fp
				else
					srcReg = (U32)gpOf(s); // SIToFP/UIToFP: gp -> xmm
				a.cvtRR(pfx, opc, w, dst, srcReg);
			}

			void fldSlot(I32 slot) { a.fldT(RBP, slot); }
			void fstpSlot(I32 slot) { a.fstpT(RBP, slot); }

			void emitX87LoadMem(const MachineInstr& in) {
				if(in.imm == -1) {
					fldSlot(in.uses[0].slot);
					return;
				}
				Reg base = gpOf(in.uses[0]);
				a.fldT(base, 0);
				fstpSlot(in.defs[0].slot);
			}

			void emitX87StoreMem(const MachineInstr& in) {
				if(in.imm == -1) {
					fstpSlot(in.defs[0].slot);
					return;
				}
				if(in.imm == -2) {
					a.fstpReg0();
					return;
				}
				Reg base = gpOf(in.uses[0]);
				fldSlot(in.uses[1].slot);
				a.fstpT(base, 0);
			}

			void emitX87LoadImmD(const MachineInstr& in) {
				a.movRegImm64(R11, (U64)in.uses[0].imm);
				a.storeMem(RBP, fl.ldScratch, R11, 8);
				a.fldL(RBP, fl.ldScratch);
				fstpSlot(in.defs[0].slot);
			}

			void emitX87FromInt(const MachineInstr& in) {
				readGp(in.uses[0], R11);
				a.storeMem(RBP, fl.ldScratch, R11, 8);
				a.fildQ(RBP, fl.ldScratch);
				fstpSlot(in.defs[0].slot);
			}

			void emitX87ToInt(const MachineInstr& in) {
				fldSlot(in.uses[0].slot);
				a.fnstcw(RBP, fl.ldScratch + 8);
				a.loadExt(R10, RBP, fl.ldScratch + 8, 2, false);
				a.movRegImm64(R11, 0x0c00);
				a.orRR(R10, R11);
				a.storeMem(RBP, fl.ldScratch + 10, R10, 2);
				a.fldcw(RBP, fl.ldScratch + 10);
				a.fistpQ(RBP, fl.ldScratch);
				a.fldcw(RBP, fl.ldScratch + 8);
				a.load64(gpOf(in.defs[0]), RBP, fl.ldScratch);
			}

			void emitX87FromSse(const MachineInstr& in) {
				if(in.imm == 80) {
					fldSlot(in.uses[0].slot);
					fstpSlot(in.defs[0].slot);
					return;
				}
				U32 sw = (U32)in.imm;
				a.storeXmm(xmmOf(in.uses[0]), RBP, fl.ldScratch, sw);
				if(sw == 4)
					a.fldD(RBP, fl.ldScratch);
				else
					a.fldL(RBP, fl.ldScratch);
				fstpSlot(in.defs[0].slot);
			}

			void emitX87ToSse(const MachineInstr& in) {
				U32 dw = (U32)in.imm;
				fldSlot(in.uses[0].slot);
				if(dw == 4)
					a.fstpD(RBP, fl.ldScratch);
				else
					a.fstpL(RBP, fl.ldScratch);
				a.loadXmm(xmmOf(in.defs[0]), RBP, fl.ldScratch, dw);
			}

			void emitX87Binary(const MachineInstr& in, U32 idx) {
				fldSlot(in.uses[0].slot);
				fldSlot(in.uses[1].slot);
				switch(idx) {
				case 0:
					a.faddp();
					break;
				case 1:
					a.fsubp();
					break;
				case 2:
					a.fmulp();
					break;
				case 3:
					a.fdivp();
					break;
				}
				fstpSlot(in.defs[0].slot);
			}

			void emitX87Neg(const MachineInstr& in) {
				fldSlot(in.uses[0].slot);
				a.fchs();
				fstpSlot(in.defs[0].slot);
			}

			void emitX87Cmp(const MachineInstr& in) {
				B32 swap = in.imm2 != 0;
				if(swap) {
					fldSlot(in.uses[0].slot); // -> st(1)
					fldSlot(in.uses[1].slot); // -> st(0)
				} else {
					fldSlot(in.uses[1].slot); // -> st(1)
					fldSlot(in.uses[0].slot); // -> st(0)
				}
				a.fucomip();
				a.fstpReg0();
				Reg d = gpOf(in.defs[0]);
				a.setcc((U8)in.imm, d);
				a.movzxByte(d, d);
			}

			void emitVaStart(const MachineInstr& in) {
				Reg ptr = gpOf(in.uses[0]);
				U32 namedGp = (U32)in.imm, namedFp = (U32)in.imm2;
				if(ptr != R10)
					a.movRR(R10, ptr);
				a.movRegImm64(R11, namedGp * 8);
				a.storeMem(R10, 0, R11, 4);
				a.movRegImm64(R11, kGpSaveBytes + namedFp * kXmmSlotBytes);
				a.storeMem(R10, 4, R11, 4);
				a.leaMem(R11, RBP, fl.overflowOff);
				a.storeMem(R10, 8, R11, 8);
				a.leaMem(R11, RBP, fl.saveArea);
				a.storeMem(R10, 16, R11, 8);
			}

			void vaFetchOverflow(I32 step) {
				a.load64(R11, R10, 8);								 // R11 = overflow_arg_area
				a.storeMem(RBP, fl.ldScratch, R11, 8); // stash address
				a.addRegImm32(R11, step);							 // advance
				a.storeMem(R10, 8, R11, 8);						 // write back overflow_arg_area
				a.load64(R11, RBP, fl.ldScratch);			 // R11 = stashed address
			}

			void vaFetch(I32 offDisp, U32 limit, I32 regStep) {
				a.loadExt(R11, R10, offDisp, 4, false); // R11 = cur offset
				a.cmpRegImm32(R11, (I32)limit);					// offset vs limit
				U32 toStack = a.jccRel32(CC_AE);				// offset >= limit -> overflow path
				a.storeMem(RBP, fl.ldScratch, R11, 8); // stash original offset
				a.addRegImm32(R11, regStep);
				a.storeMem(R10, offDisp, R11, 4); // write advanced offset
				a.load64(R11, RBP, fl.ldScratch); // R11 = original offset
				a.addRegMem(R11, R10, 16);				// R11 += reg_save_area base
				U32 done = a.jmpRel32();
				a.patchRel32(toStack, a.here());
				vaFetchOverflow(8);
				a.patchRel32(done, a.here());
			}

			void emitVaArg(const MachineInstr& in) {
				Reg ptr = gpOf(in.uses[0]);
				if(ptr != R10)
					a.movRR(R10, ptr);
				VaArgKind kind = (VaArgKind)in.imm;
				U32 width = (U32)(in.imm2 & 0xffffffff);
				B32 sign = (in.imm2 >> 32) != 0;
				if(kind == VaArgKind::X87) {
					vaFetchOverflow(16);
					a.fldT(R11, 0);
					fstpSlot(in.defs[0].slot);
					return;
				}
				if(kind == VaArgKind::Sse) {
					vaFetch(4, kRegSaveBytes, (I32)kXmmSlotBytes);
					a.loadXmm(xmmOf(in.defs[0]), R11, 0, width);
					return;
				}
				vaFetch(0, kGpSaveBytes, 8);
				a.loadExt(gpOf(in.defs[0]), R11, 0, width, sign);
			}

			void emitCall(const MachineInstr& in) {
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
				U32 firstStack = targetIdx + 1;

				B32 pad = (stackBytes & 15) != 0;
				if(pad)
					a.subRegImm32(RSP, 8);
				for(U32 k = in.uses.size(); k-- > firstStack;) {
					const MachineOperand& u = in.uses[k];
					if(u.kind == MachineOperand::Kind::FrameSlot) {
						if(u.width == 16) {
							a.subRegImm32(RSP, 16);
							fldSlot(u.slot);
							a.fstpT(RSP, 0);
						} else {
							a.load64(R11, RBP, u.slot);
							a.push(R11);
						}
					} else if(X86Target::isXmm(u.phys)) {
						a.subRegImm32(RSP, 8);
						a.storeXmm(xmmOf(u), RSP, 0, u.width);
					} else {
						a.push(gpOf(u));
					}
				}

				if(indirect)
					a.callReg(R11);
				else
					a.callSym(in.uses[targetIdx].sym);

				U32 popBytes = (U32)stackBytes + (pad ? 8 : 0);
				if(popBytes)
					a.addRegImm32(RSP, (I32)popBytes);
			}

			static PhysReg gpReg11() { return X86Target::kGpBase + (PhysReg)R11; }

			void recordFix(U32 dispAt, I32 targetBlock) { fixes.push_back({dispAt, targetBlock}); }

			void emitRet(const MachineInstr&) {
				for(U32 i = 0; i < calleeSaved.size(); ++i)
					a.load64(toGp(calleeSaved[i]), RBP, calleeBase - (I32)(8 * (i + 1)));
				a.leave();
				a.ret();
			}

			void emitJmp(const MachineInstr& in, I32 fallthrough) {
				I32 target = in.uses[0].block;
				if(target == fallthrough)
					return;
				recordFix(a.jmpRel32(), target);
			}

			void emitBr(const MachineInstr& in, I32 fallthrough) {
				Reg p = gpOf(in.uses[0]);
				I32 thenB = in.uses[1].block;
				I32 elseB = in.uses[2].block;
				a.testRR(p, p);
				recordFix(a.jccRel32(CC_NE), thenB);
				if(elseB != fallthrough)
					recordFix(a.jmpRel32(), elseB);
			}

			void emitInst(const MachineInstr& in, I32 fallthrough) {
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
				case X86Op::LoadFrame:
					emitLoadFrame(in);
					return;
				case X86Op::FrameAddr:
					emitFrameAddr(in);
					return;
				case X86Op::Load:
					emitLoad(in);
					return;
				case X86Op::Store:
					emitStore(in);
					return;
				case X86Op::Add:
					emitAlu(in, 0x01);
					return;
				case X86Op::Sub:
					emitAlu(in, 0x29);
					return;
				case X86Op::Mul:
					emitMul(in);
					return;
				case X86Op::And:
					emitAlu(in, 0x21);
					return;
				case X86Op::Or:
					emitAlu(in, 0x09);
					return;
				case X86Op::Xor:
					emitAlu(in, 0x31);
					return;
				case X86Op::Neg:
					emitNegNot(in, true);
					return;
				case X86Op::Not:
					emitNegNot(in, false);
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
				case X86Op::Movzx:
					a.movzxByte(gpOf(in.defs[0]), gpOf(in.uses[0]));
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
					emitFArith(in, kSseOp[0]);
					return;
				case X86Op::FSub:
					emitFArith(in, kSseOp[1]);
					return;
				case X86Op::FMul:
					emitFArith(in, kSseOp[2]);
					return;
				case X86Op::FDiv:
					emitFArith(in, kSseOp[3]);
					return;
				case X86Op::FNeg:
					emitFNeg(in);
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
					emitX87Binary(in, 0);
					return;
				case X86Op::X87Sub:
					emitX87Binary(in, 1);
					return;
				case X86Op::X87Mul:
					emitX87Binary(in, 2);
					return;
				case X86Op::X87Div:
					emitX87Binary(in, 3);
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

			void prologue() {
				a.push(RBP);
				a.movRR(RBP, RSP);
				if(frameSize)
					a.subRegImm32(RSP, (I32)frameSize);
				for(U32 i = 0; i < calleeSaved.size(); ++i)
					a.storeMem(RBP, calleeBase - (I32)(8 * (i + 1)), toGp(calleeSaved[i]), 8);
				if(fl.variadic) {
					for(U32 i = 0; i < kMaxIntArgs; ++i)
						a.storeMem(RBP, fl.saveArea + (I32)(i * 8), kIntArgRegs[i], 8);
					a.testRR(RAX, RAX);
					U32 skip = a.jccRel32(CC_E);
					for(U32 i = 0; i < kMaxXmmArgs; ++i)
						a.storeXmm(i, RBP, fl.saveArea + (I32)kGpSaveBytes + (I32)(i * kXmmSlotBytes), 8);
					a.patchRel32(skip, a.here());
				}
			}

			void run() {
				calleeBase = -(I32)fn.frameBytes;
				frameSize = (fn.frameBytes + 8u * (U32)calleeSaved.size() + 15u) & ~15u;
				blockOffset.assign(fn.blocks.size(), 0);
				prologue();
				for(U32 bi = 0; bi < fn.blocks.size(); ++bi) {
					const MachineBlock& blk = fn.blocks[bi];
					if(blk.id < 0)
						continue;
					blockOffset[blk.id] = a.here();
					I32 fallthrough = (bi + 1 < fn.blocks.size()) ? (I32)fn.blocks[bi + 1].id : -1;
					for(const MachineInstr& in : blk.insts)
						emitInst(in, fallthrough);
				}
				for(const JumpFix& f : fixes)
					a.patchRel32(f.dispAt, blockOffset[f.targetBlock]);
			}
		};
	} // namespace

	U32 X86LowerPass::runOnMachineFunction(const Function& fn,
																				 MachineFunc& mf,
																				 const TargetInfo& /*target*/) {
		X86Lower lower(fn);
		mf = lower.lower(); // the frame layout rides along on mf.aux
		return 1;
	}

	void X86EncodePass::emitGlobal(ElfObject& elf, const Module& mod, const Global* g) {
		const List<U8>& init = g->getInit();
		U32 size = g->getType()->byteSize(mod.pointerBytes());
		if(size == 0)
			size = (U32)init.size();
		if(size == 0)
			size = 1;

		B32 allZero = g->getRelocs().empty() &&
									std::all_of(init.begin(), init.end(), [](U8 v) { return v == 0; });
		ElfObject::Section sec =
				allZero ? ElfObject::Bss : (g->isConstant() ? ElfObject::Rodata : ElfObject::Data);
		elf.align(sec, 8);

		U32 off;
		if(allZero) {
			off = elf.appendZero(sec, size);
		} else {
			List<U8> img(size, 0);
			std::copy_n(init.begin(), init.size() < size ? init.size() : size, img.begin());
			off = elf.append(sec, img.data(), size);
		}
		elf.defineSymbol(g->getName(), sec, off, true, false);

		for(const Reloc& r : g->getRelocs())
			elf.addReloc(sec, off + r.offset, r.symbol, ElfReloc::Abs64, r.addend);
	}

	B32 X86EncodePass::run(Module& mod, MachineModule& mm, const TargetInfo& /*target*/) {
		ElfObject elf;

		for(const Global* g : mod.globals())
			emitGlobal(elf, mod, g);

		for(const Function* fn : mod) {
			MachineFunc& mf = mm.get(fn);

			const X86FrameLayout& fl = *static_cast<const X86FrameLayout*>(mf.aux.get());

			List<U8> code;				 // emitted machine code bytes
			List<AsmReloc> relocs; // relocations into code
			Asm a(code, relocs);
			Encoder enc(mf, fl, a, mf.usedCalleeSaved);
			enc.run();

			elf.align(ElfObject::Text, 16);
			U32 off = elf.append(ElfObject::Text, code.data(), (U32)code.size());
			elf.defineSymbol(fn->getName(), ElfObject::Text, off, true, true);
			for(const AsmReloc& r : relocs)
				elf.addReloc(ElfObject::Text, off + r.offset, r.symbol, r.kind, r.addend);
		}

		elf.write(*os);
		return false;
	}

	RegAllocHooks X86Target::regAllocHooks() const { return x86RegAllocHooks(); }
} // namespace rat
