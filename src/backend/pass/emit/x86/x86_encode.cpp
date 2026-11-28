#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/type.h"
#include "target/object_file.h"
#include "target/target.h"

#include <functional>

namespace rat {
	Reg X86EncodePass::toGp(PhysReg p) { return (Reg)(p - X86Target::kGpBase); }
	Reg X86EncodePass::gpOf(const MachineOperand& o) { return toGp(o.phys); }
	U32 X86EncodePass::xmmOf(const MachineOperand& o) { return o.phys - X86Target::kXmmBase; }
	U32 X86EncodePass::regNum(const MachineOperand& o) {
		return X86Target::isXmm(o.phys) ? xmmOf(o) : (U32)gpOf(o);
	}

	Mem X86EncodePass::memOf(const MachineInstr& in, U32 indexUse) {
		if(in.uses[0].isFrameSlot())
			return {RBP, in.uses[0].slot};
		Reg base = gpOf(in.uses[0]);
		if(!(in.imm2 & detail::kSibIndexed))
			return {base, (I32)in.imm};
		U32 scaleLog2 = detail::sibScaleLog2(in.imm2);
		return indexedMem(base, gpOf(in.uses[indexUse]), scaleLog2, (I32)in.imm);
	}

	void X86EncodePass::readGp(const MachineOperand& o, Reg r) {
		if(o.isImm())
			a->movImm64(r, (U64)o.imm);
		else if(o.isPhys()) {
			if(gpOf(o) != r)
				a->rr(enc::kMovW, gpOf(o), r);
		} else if(o.isFrameSlot())
			a->load(r, {RBP, o.slot}, 8, false);
	}

	void X86EncodePass::emitVaStart(const MachineInstr& in) {
		readGp(in.uses[0], R10);
		if(conv->vaList == X86VaList::CharPtr) {
			a->rm(enc::kLeaW, R11, {RBP, fl->overflowOff});
			return a->store({R10, 0}, R11, 8);
		}
		a->movImm64(R11, (U32)in.imm * 8);
		a->store({R10, 0}, R11, 4);
		a->movImm64(R11, conv->gpSaveBytes + (U32)in.imm2 * conv->sseSlotBytes);
		a->store({R10, 4}, R11, 4);
		a->rm(enc::kLeaW, R11, {RBP, fl->overflowOff});
		a->store({R10, 8}, R11, 8);
		a->rm(enc::kLeaW, R11, {RBP, fl->saveArea});
		a->store({R10, 16}, R11, 8);
	}

	// small aggregates go through the xmm scratch, large ones by rep movsq with rsi rdi rcx parked
	void X86EncodePass::copyByval(const MachineOperand& src, U32 bytes, I32 off) {
		if(bytes <= kByvalInlineBytes) {
			U32 chunk = conv->sseVolatileCount - 1;
			readGp(src, R10);
			for(U32 k = 0; k < bytes; k += 8) {
				a->loadXmm(chunk, {R10, (I32)k}, 8);
				a->storeXmm(chunk, {RSP, off + (I32)k}, 8);
			}
			return;
		}
		U32 keepRsi = conv->sseVolatileCount - 1;
		U32 keepRdi = conv->sseVolatileCount - 2;
		a->rr(enc::kMovqToXmm, keepRsi, RSI);
		a->rr(enc::kMovqToXmm, keepRdi, RDI);
		a->rr(enc::kMovW, RCX, R10);
		readGp(src, RSI);
		a->rm(enc::kLeaW, RDI, {RSP, off});
		movImm32(RCX, (U32)alignUp(bytes, 8) / 8);
		a->opReg({0xf3, 0, 0xa5, 1}, 0); // rep movsq
		a->rr(enc::kMovW, R10, RCX);
		a->rr(enc::kMovqFromXmm, keepRdi, RDI);
		a->rr(enc::kMovqFromXmm, keepRsi, RSI);
	}

	// the sym use, or the r11 use of an indirect call. register args come before it, stack args after
	U32 X86EncodePass::callTargetUse(const MachineInstr& in) {
		for(U32 i = 0; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(u.kind == MachineOperand::Kind::Sym || (u.isPhys() && u.phys == detail::gpPhys(R11)))
				return i;
		}
		assert(false && "call without target");
		return 0;
	}

	// a stack arg followed by an imm is a by-value aggregate
	void X86EncodePass::storeStackArgs(const MachineInstr& in, U32 targetUse) {
		I32 off = (I32)conv->shadowBytes;
		for(U32 i = targetUse + 1; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(i + 1 < in.uses.size() && in.uses[i + 1].isImm()) {
				U64 byval = (U64)in.uses[++i].imm; // stack offset in the high half, bytes in the low
				U32 bytes = (U32)byval;
				off = (I32)conv->shadowBytes + (I32)(byval >> 32);
				copyByval(u, bytes, off);
				off += (I32)alignUp(bytes, 8);
				continue;
			}
			if(u.isFrameSlot() && u.width == 16) { // by-value x87
				off = (I32)alignUp((U32)off, 16);
				fldSlot(u);
				x87Mem(0xdb, 7, {RSP, off});
				off += 16;
				continue;
			}
			if(u.isFrameSlot()) {
				a->load(R10, {RBP, u.slot}, 8, false);
				a->store({RSP, off}, R10, 8);
			} else if(X86Target::isXmm(u.phys)) {
				a->storeXmm(xmmOf(u), {RSP, off}, 8);
			} else {
				a->store({RSP, off}, gpOf(u), 8);
			}
			off += 8;
		}
	}

	// variadic callees read every register argument from the gp set
	void X86EncodePass::mirrorSseArgs(const MachineInstr& in, U32 targetUse) {
		for(U32 i = targetUse; i-- > 0;) {
			const MachineOperand& u = in.uses[i];
			if(!u.isPhys() || !X86Target::isXmm(u.phys) || xmmOf(u) >= conv->gpArgCount)
				continue;
			a->rr(enc::kMovqFromXmm, xmmOf(u), conv->gpArgs[xmmOf(u)]);
		}
	}

	void X86EncodePass::emitCall(const MachineInstr& in) {
		B32 indirect = in.imm2 != 0;
		U32 targetUse = callTargetUse(in);
		U32 total = (U32)alignUp((U32)in.imm + conv->shadowBytes, 16);
		U32 areaAlign = (U32)(in.imm >> 32);
		if(areaAlign > 16)
			realignArgs(total, areaAlign);
		else if(total)
			a->aluImm(ALU_SUB, RSP, (I32)total);
		storeStackArgs(in, targetUse);
		if(conv->dupSseArgsInGp)
			mirrorSseArgs(in, targetUse);
		if(indirect)
			a->rr(enc::kGroup5, 2, R11); // call
		else
			a->callSym(in.uses[targetUse].sym());
		if(areaAlign > 16)
			a->load(RSP, {RSP, (I32)total}, 8, false);
		else if(total)
			a->aluImm(ALU_ADD, RSP, (I32)total);
	}

	// rsp aligned for the args, the old rsp waits above them for the restore
	void X86EncodePass::realignArgs(U32 total, U32 align) {
		a->rr(enc::kMovW, RSP, R10);
		a->aluImm(ALU_SUB, RSP, (I32)total + 8);
		a->aluImm(ALU_AND, RSP, -(I32)align);
		a->store({RSP, (I32)total}, R10, 8);
	}

	void X86EncodePass::emitRet() {
		if(omitFrame)
			return a->b(0xc3); // ret
		const List<PhysReg>& saved = fn->usedCalleeSaved;
		// dynamic allocas move rsp, so re-point it at the save area first
		if(hasDynAlloca && !saved.empty())
			a->rm(enc::kLeaW, RSP, {RBP, -(I32)(frameSize + 8u * (U32)saved.size())});
		for(U32 i = (U32)saved.size(); i-- > 0;)
			a->opReg(enc::kPop, toGp(saved[i]));
		// the cfa falls back to rsp once leave drops the frame, code after ret is still framed
		a->b(0xc9); // leave
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RSP), 8);
		a->b(0xc3); // ret
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RBP), detail::kCfaFromRbp);
	}

	void X86EncodePass::emitSwitchJump(const MachineInstr& in) {
		// lea base, [rip + table] -> movsxd entry, [base + sel*4] -> add entry, base -> jmp entry
		Reg sel = gpOf(in.uses[0]);
		// a spilled selector reloads into scratch r10/r11, route around it
		Reg base = sel == R10 ? R11 : R10;
		Reg entry = base == R10 ? R11 : R10; // may alias sel, read before written
		PendingTable t;
		t.leaDispAt = a->rip(enc::kLeaW, base);
		for(U32 i = 1; i < (U32)in.uses.size(); ++i)
			t.targets.push_back(in.uses[i].block);
		a->rm(enc::kMovsxdW, entry, indexedMem(base, sel, 2, 0));
		a->rr(enc::kAddW, base, entry);
		a->rr(enc::kGroup5, 4, entry); // jmp
		tables.push_back(std::move(t));
	}

	void X86EncodePass::emitBr(const MachineInstr& in, I32 fallthrough) {
		U8 cc;
		I32 thenB, elseB;
		if(in.imm2) { // fused, the preceding cmp set the flags, cc in imm
			cc = (U8)in.imm;
			thenB = in.uses[0].block;
			elseB = in.uses[1].block;
		} else {
			testReg(in.uses[0]);
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
		Reg dGp = in.defs.empty() ? RAX : gpOf(in.defs[0]);
		B32 w = !in.defs.empty() && in.defs[0].width == 8;
		switch(op) {
		case X86Op::Copy:
			return emitCopy(in);
		case X86Op::LoadImm:
			return emitLoadImm(in);
		case X86Op::LoadSym:
			return a->ripSym(enc::kLeaW, dGp, in.uses[0].sym());
		case X86Op::FrameAddr:
			return a->rm(enc::kLeaW, dGp, {RBP, (I32)in.imm});
		case X86Op::RetAddr:
			return a->load(dGp, {RBP, 8}, 8, false);
		case X86Op::StackAlloc:
			return emitStackAlloc(in);
		case X86Op::StackSave:
			return a->rr(enc::kMovW, RSP, dGp);
		case X86Op::StackRestore:
			readGp(in.uses[0], R10);
			return a->rr(enc::kMovW, R10, RSP);
		case X86Op::SetJmp:
			return emitSetJmp(in);
		case X86Op::LongJmp:
			return emitLongJmp(in);
		case X86Op::Lea:
			return a->rm(enc::kLeaW, dGp, memOf(in, 1));
		case X86Op::Load:
			assert((!in.uses[0].isFrameSlot() || in.defs[0].width == 8) && "a slot load moves 8 bytes");
			return a->load(dGp, memOf(in, 1), in.defs[0].width, (in.imm2 & detail::kSibSign) != 0);
		case X86Op::Store:
			return emitStore(in);
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor: {
			static constexpr U8 kAlu[] = {0x01, 0x29, 0, 0x21, 0x09, 0x31}; // add sub (mul) and or xor
			static_assert((U32)X86Op::Xor - (U32)X86Op::Add + 1 == 6, "kAlu must cover Add..Xor");
			return emitAlu(in, kAlu[(U32)op - (U32)X86Op::Add]);
		}
		case X86Op::Mul:
			if(in.uses[1].isImm())
				return a->rrImm(0x6b, 0x69, dGp, gpOf(in.uses[0]), (I32)in.uses[1].imm, w);
			return a->rr({0, 0x0f, 0xaf, w}, dGp, gpOf(in.uses[1])); // imul
		case X86Op::Neg:
		case X86Op::Not:
			return a->rr({0, 0, 0xf7, w}, op == X86Op::Neg ? 3 : 2, dGp); // neg /3, not /2
		case X86Op::Shl:
		case X86Op::AShr:
		case X86Op::LShr: {
			static constexpr U8 kShift[] = {4, 7, 5}; // group-2 /digit for shl, sar, shr
			U8 ext = kShift[(U32)op - (U32)X86Op::Shl];
			if(!in.uses[1].isImm())
				return a->rr({0, 0, 0xd3, w}, ext, dGp); // by cl
			return shiftImm(ext, dGp, (U8)(in.uses[1].imm & 63), w);
		}
		case X86Op::Rotl:
		case X86Op::Rotr:
			return shiftImm(op == X86Op::Rotr, dGp, (U8)in.uses[1].imm, in.imm == 64); // rol /0, ror /1
		case X86Op::SDiv:
		case X86Op::SRem:
		case X86Op::UDiv:
		case X86Op::URem:
			return emitDiv(in, op == X86Op::SDiv || op == X86Op::SRem);
		case X86Op::BitScanF:
		case X86Op::BitScanR: {
			U8 opc = op == X86Op::BitScanR ? 0xbd : 0xbc;
			return a->rr({0, 0x0f, opc, in.imm == 64}, dGp, gpOf(in.uses[0]));
		}
		case X86Op::Cmp:
			if(in.uses[1].isImm() && in.uses[1].imm == 0)
				return testReg(in.uses[0]); // shorter, same flags for eq/ne/sign
			return emitAlu(in, 0x39);			// cmp
		case X86Op::SetCC:
			return setccExt((U8)in.imm, dGp);
		case X86Op::CMov:
			return a->rr({0, 0x0f, (U8)(0x40 + in.imm), 1}, dGp, gpOf(in.uses[1])); // cmovcc
		case X86Op::MaskBits:
		case X86Op::SignExtBits:
			return emitExtBits(in, op == X86Op::SignExtBits);
		case X86Op::Bswap:
			return a->opReg({0, 0x0f, 0xc8, in.imm == 64}, dGp); // bswap
		case X86Op::Ud2:
			return twoBytes(0x0f, 0x0b);
		case X86Op::Prefetch:
			return a->rm({0, 0x0f, 0x18}, (U8)in.imm2, {gpOf(in.uses[0]), (I32)in.imm}); // prefetch
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
		default:
			return emitSse(in);
		}
	}

	void X86EncodePass::emitSse(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		switch(op) {
		case X86Op::FLoad:
			return emitFLoad(in);
		case X86Op::FStore:
			return a->storeXmm(xmmOf(in.uses[1]), memOf(in, 2), in.uses[1].width);
		case X86Op::FNeg:
		case X86Op::FAbs:
			return emitFSign(in, op == X86Op::FAbs);
		case X86Op::FCmp:
			emitFCmpFlags(in);
			return setccExt((U8)in.imm, gpOf(in.defs[0]));
		case X86Op::FCmpFlags:
			return emitFCmpFlags(in);
		case X86Op::Sse: {
			const MachineOperand& src = in.uses[in.uses.size() >= 2 ? 1 : 0];
			a->rr(detail::unpackEnc(in.imm), regNum(in.defs[0]), regNum(src));
			if(in.imm & detail::kSseImm8)
				a->b((U8)in.imm2);
			return;
		}
		case X86Op::VShiftImm:
			return sseShift((U32)in.imm, (U8)(in.imm2 >> 8), xmmOf(in.defs[0]), (U8)in.imm2);
		case X86Op::VSplat:
			return emitVSplat(in);
		case X86Op::VExtract:
			return emitVExtract(in);
		default:
			return emitX87(in);
		}
	}

	void X86EncodePass::emitX87(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		switch(op) {
		case X86Op::X87LoadMem:
			if(in.imm == -1)
				return fldSlot(in.uses[0]);
			x87Mem(0xdb, 5, {gpOf(in.uses[0]), 0});
			return fstpSlot(in.defs[0]);
		case X86Op::X87StoreMem:
			return emitX87StoreMem(in);
		case X86Op::X87LoadImmD:
		case X86Op::X87FromInt:
			readGp(in.uses[0], R11);
			a->store({RBP, fl->ldScratch}, R11, 8);
			if(op == X86Op::X87FromInt)
				x87Mem(0xdf, 5, {RBP, fl->ldScratch}); // fild m64
			else
				x87Mem(0xdd, 0, {RBP, fl->ldScratch}); // fld m64fp
			return fstpSlot(in.defs[0]);
		case X86Op::X87ToInt:
			return emitX87ToInt(in);
		case X86Op::X87FromSse:
			return emitX87FromSse(in);
		case X86Op::X87ToSse:
			fldSlot(in.uses[0]);
			x87Mem(in.imm == 4 ? 0xd9 : 0xdd, 3, {RBP, fl->ldScratch}); // fstp m32fp, m64fp
			return a->loadXmm(xmmOf(in.defs[0]), {RBP, fl->ldScratch}, (U32)in.imm);
		case X86Op::X87Add:
		case X86Op::X87Sub:
		case X86Op::X87Mul:
		case X86Op::X87Div:
			return emitX87Binary(in, (U32)op - (U32)X86Op::X87Add);
		case X86Op::X87Neg:
			fldSlot(in.uses[0]);
			twoBytes(0xd9, 0xe0); // fchs
			return fstpSlot(in.defs[0]);
		case X86Op::X87Cmp:
			return emitX87Cmp(in);
		default:
			assert(false && "no encoding for this op");
		}
	}

	void X86EncodePass::prologue() {
		if(omitFrame)
			return; // frameless leaf
		a->opReg(enc::kPush, RBP);
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RSP), detail::kCfaFromRbp);
		markCfi(CfiOp::Kind::Offset, detail::gpPhys(RBP), -detail::kCfaFromRbp);
		a->rr(enc::kMovW, RSP, RBP);
		markCfi(CfiOp::Kind::DefCfa, detail::gpPhys(RBP), detail::kCfaFromRbp);
		U32 remaining = frameSize;
		if(conv->probeStack && frameSize > 4096) {
			// touch each page so the guard page grows
			for(; remaining >= 4096; remaining -= 4096) {
				a->aluImm(ALU_SUB, RSP, 4096);
				a->rm({0, 0, 0x83}, ALU_OR, {RSP, 0}); // or dword [rsp], 0
				a->b(0);
			}
		}
		if(remaining)
			a->aluImm(ALU_SUB, RSP, (I32)remaining);
		// callee saves below the frame slots, via push/pop
		I32 saveAt = -detail::kCfaFromRbp - (I32)frameSize;
		for(PhysReg r : fn->usedCalleeSaved) {
			a->opReg(enc::kPush, toGp(r));
			saveAt -= 8;
			markCfi(CfiOp::Kind::Offset, r, saveAt);
		}
		if(!fl->variadic)
			return;
		// spill the positional register arguments into their home slots
		B32 win64 = conv->vaList == X86VaList::CharPtr;
		I32 gpBase = win64 ? conv->homeOff : fl->saveArea;
		for(U32 i = 0; i < conv->gpArgCount; ++i)
			a->store({RBP, gpBase + (I32)(i * 8)}, conv->gpArgs[i], 8);
		if(win64)
			return;
		a->rr(enc::kTestW, RAX, RAX);
		U32 skip = a->jccRel32(CC_E);
		I32 sseBase = fl->saveArea + (I32)conv->gpSaveBytes;
		for(U32 i = 0; i < conv->sseArgCount; ++i)
			a->storeXmm(i, {RBP, sseBase + (I32)(i * conv->sseSlotBytes)}, 8);
		a->patchRel32(skip, a->here());
	}

	void X86EncodePass::markCfi(CfiOp::Kind kind, PhysReg reg, I32 value) {
		if(encodedFunc)
			encodedFunc->cfi.push_back({a->here(), kind, detail::kDwarfRegs[reg], value});
	}

	// the decl row at 0 always gets a second row, gdb puts the breakpoint past the prologue there
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
			I32 cfaOff = kNoFrameSlot;
			if(v.slot) {
				I32 slotOff = fl->allocOff[v.slot->getId()];
				if(slotOff != detail::kNoSlot)
					cfaOff = slotOff - detail::kCfaFromRbp;
			}
			encodedFunc->varFrameOff.push_back(cfaOff);
		}
	}

	B32 X86EncodePass::touchesFrame(const MachineOperand& o) {
		return o.isFrameSlot() || (o.isPhys() && gpOf(o) == RBP);
	}

	B32 X86EncodePass::needsFrame(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		B32 isX87 = op >= X86Op::X87LoadMem && op <= X86Op::X87Cmp;
		B32 isSetLongJmp = op == X86Op::SetJmp || op == X86Op::LongJmp;
		B32 readsRbp = op == X86Op::FrameAddr || op == X86Op::VaStart || op == X86Op::RetAddr;
		if(in.isCall || readsRbp || isSetLongJmp || isX87)
			return true;
		for(const MachineOperand& o : in.defs)
			if(touchesFrame(o))
				return true;
		for(const MachineOperand& o : in.uses)
			if(touchesFrame(o))
				return true;
		return false;
	}

	// slots in [rbp-frameSize, rbp), saves pushed below, 16-aligned. returns the instr count
	U32 X86EncodePass::planFrame() {
		U32 saveBytes = 8u * (U32)fn->usedCalleeSaved.size();
		frameSize = (U32)alignUp(fn->frameBytes + saveBytes, 16) - saveBytes;
		hasDynAlloca = false;
		B32 framey = fn->frameBytes != 0 || saveBytes != 0 || fl->variadic;
		U32 instCount = 0;
		for(const MachineBlock& blk : fn->blocks)
			for(const MachineInstr& in : blk.insts) {
				X86Op op = (X86Op)in.op;
				if(op == X86Op::StackAlloc || op == X86Op::StackSave || op == X86Op::StackRestore)
					hasDynAlloca = true;
				framey = framey || needsFrame(in);
				++instCount;
			}
		omitFrame = !framey && !hasDynAlloca;
		return instCount;
	}

	// jump tables hold 4-byte offsets from the table base, so no relocations
	void X86EncodePass::patchJumps() {
		for(const PendingTable& t : tables) {
			U32 base = a->here();
			a->patchRel32(t.leaDispAt, base);
			for(I32 tb : t.targets)
				a->d32(blockOffset[tb] - base);
		}
		for(const JumpFix& jf : fixes)
			a->patchRel32(jf.dispAt, blockOffset[jf.targetBlock]);
	}

	void X86EncodePass::encodeFunction(const MachineFunc& f, Asm& asm_) {
		fn = &f;
		fl = static_cast<const X86FrameLayout*>(f.aux.get());
		a = &asm_;
		fixes.clear();
		tables.clear();
		U32 instCount = planFrame();
		a->code.reserve((U64)instCount * 16u + 64u); // cheap upper estimate
		blockOffset.assign(detail::blockIdBound(f), 0);
		trackVars(instCount);
		prologue();
		VarState vars;
		for(U32 bi = 0; bi < f.blocks.size(); ++bi) {
			const MachineBlock& blk = f.blocks[bi];
			if(blk.id < 0)
				continue;
			if(blk.align)
				a->alignNop(blk.align);
			blockOffset[blk.id] = a->here();
			walkBlock(bi, vars, true);
		}
		patchJumps();
	}

	void X86EncodePass::emitGlobal(ObjectFile& obj, const Global* g, U32 ptrBytes) {
		const List<U8>& init = g->getInit();
		const List<Reloc>& relocs = g->getRelocs();
		U32 size = g->getType()->byteSize(ptrBytes);
		if(size == 0)
			size = (U32)init.size();
		if(size == 0)
			size = 1;

		B32 allZero = relocs.empty() && std::all_of(init.begin(), init.end(), std::logical_not<U8>());
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

		for(const Reloc& r : relocs)
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
