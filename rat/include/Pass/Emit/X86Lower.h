#ifndef RAT_PASS_EMIT_X86LOWER_H
#define RAT_PASS_EMIT_X86LOWER_H

#include "Core.h"

#include "CodeGen/Schedule.h"
#include "IR/Opcode.h"
#include "Pass/Emit/X86Op.h"
#include "Support/Pass.h"

namespace rat {
	struct AllocNode;
	struct BinaryNode;
	struct CallNode;
	struct CompareNode;
	struct ConvertNode;
	struct Function;
	struct LoadNode;
	struct Node;
	struct ProjNode;
	struct ReturnNode;
	struct StoreNode;
	struct Type;
	struct UnaryNode;

	struct X86LowerPass : MachinePass {
		const C8* name() const override { return "x86-lower"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		U32 runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target);

		void reset(const Function& f, Schedule& s, MachineFunc& o, X86FrameLayout& layout);
		void lowerFunction();

		static PhysReg gpReg(Reg r);
		static PhysReg xmmReg(U32 n);
		static B32 isFloatTy(const Type* t);
		static B32 isX87Ty(const Type* t);
		static B32 isSseTy(const Type* t);
		static U32 intBits(const Type* t);
		static B32 isIntCompare(Node* n);
		static B32 immOf(Node* n, I64& out);
		static B32 branchOnlyCompare(Node* n);
		static U32 opWidth(const Type* t);
		static String libcName(const String& callee);

		I32 reserve(U32 bytes);
		void needScratch();
		void layout();
		void layoutVariadic();
		void layoutVariadicWin64();
		U32 classOf(const Type* t) const;
		VReg fresh(U32 cls);
		I32 x87SlotOf(const Node* n);
		VReg vregFor(const Node* n);
		void emit(MachineInstr in);
		MachineInstr& inst(X86Op op,
											 U32 cls,
											 List<MachineOperand> defs,
											 List<MachineOperand> uses,
											 I64 imm = 0,
											 I64 imm2 = 0);
		void copy(MachineOperand dst, MachineOperand src, U32 cls);
		MachineInstr& def1(X86Op op, VReg dst, U32 cls, List<MachineOperand> uses);
		VReg gpValue(Node* n);

		struct AddrMatch {
			Node* base = nullptr;
			Node* index = nullptr;
			Node* scaleNode = nullptr;
			U32 scaleLog2 = 0;
			I32 disp = 0;
			B32 hasIndex = false;
		};
		struct AddrParts {
			VReg base = 0;
			VReg index = 0;
			U32 scaleLog2 = 0;
			I32 disp = 0;
			B32 hasIndex = false;
		};
		B32 scaleOf(Node* n, Node*& idx, U32& scaleLog2);
		AddrMatch decodeAddr(Node* ptr);
		AddrParts matchAddr(Node* ptr);
		B32 addressOnlyAdd(Node* n);
		B32 addressOnlyScale(Node* n);
		VReg storeAddr(const AddrParts& a);
		I64 sibBits(I64 sign, const AddrParts& a);
		VReg sseValue(Node* n);
		I32 x87Value(Node* n);
		void emitStore(StoreNode* s);
		void emitLoad(LoadNode* l);
		void emitAlloc(AllocNode* al);
		void twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs);
		void maskBits(VReg d, U32 bits);
		void signExtBits(VReg d, U32 bits);
		void emitDivLike(BinaryNode* n, X86Op op);
		void emitShift(BinaryNode* n, X86Op op);
		void emitBinary(BinaryNode* n);
		void emitFloatBinary(BinaryNode* n);
		void emitX87Binary(BinaryNode* n, U32 idx);
		void emitUnary(UnaryNode* n);
		void emitCompare(CompareNode* n);
		void emitFloatCompare(CompareNode* n);
		static I64 cvtDesc(U8 pfx, U8 opc, B32 w);
		void emitConvert(ConvertNode* n);
		void emitConvertX87(ConvertNode* n, Node* src, Opcode op);
		List<PhysReg> callerSavedClobbers() const;
		void emitCall(CallNode* c);
		void emitCallSysV(CallNode* c);
		void emitCallWin64(CallNode* c);
		VReg x87ByRefArg(Node* arg);
		void emitPrologue();
		void emitPrologueSysV();
		void emitPrologueWin64();
		void loadStackParam(ProjNode* p, Type* t, I32 disp);
		void emitVaStart(CallNode* c);
		void emitVaArg(CallNode* c);
		void emitNode(Node* n);
		void emitReturn(ReturnNode* r);
		void emitPhiCopies(I32 targetBlock, I32 predIdx);
		void emitTerminator(I32 b);
	private:
		const Function* fn = nullptr;
		B32 winAbi = false;
		U32 ptrBytes = 8;
		Schedule* sched = nullptr;
		MachineFunc* out = nullptr;
		X86FrameLayout* fl = nullptr;
		Map<const Node*, VReg> vregOf;
		Map<const Node*, I32> x87Slot;
		Map<const Node*, I32> allocOff;
		MachineBlock* mb = nullptr;
	};
} // namespace rat

#endif
