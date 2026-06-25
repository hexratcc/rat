// x86/64 code emitter, lowers the IR to a system V ELF relocatable
// object written to the output stream. Each function is scheduled into basic
// blocks, given a stack frame (every value lives in a memory slot; no register
// allocation), and emitted as machine code via the Asm assembler, globals are
// laid out into the data/rodata/bss sections

#ifndef RAT_PASS_EMIT_X86EMITTER_H
#define RAT_PASS_EMIT_X86EMITTER_H

#include "CodeGen/Schedule.h"
#include "Core.h"
#include "IR/Opcode.h"
#include "Pass/Pass.h"
#include "Target/X86Asm.h"

namespace rat {
	struct AllocNode;
	struct BinaryNode;
	struct CallNode;
	struct CompareNode;
	struct ConvertNode;
	struct ElfObject;
	struct Function;
	struct Global;
	struct LoadNode;
	struct Module;
	struct Node;
	struct PhiNode;
	struct ProjNode;
	struct StoreNode;
	struct Type;
	struct UnaryNode;

	struct X86EmitterPass : Pass {
		// system V AMD64 calling convention parameters
		static constexpr Reg kIntArgRegs[6] = {RDI, RSI, RDX, RCX, R8, R9}; // integer arg registers
		static constexpr U32 kMaxIntArgs = 6;																// GP registers for args
		static constexpr U32 kMaxXmmArgs = 8;																// XMM registers for args

		static constexpr U32 kGpSaveBytes = kMaxIntArgs * 8; // GP register save area size
		static constexpr U32 kXmmSlotBytes = 16;						 // per-XMM slot in save area
		static constexpr U32 kRegSaveBytes = kGpSaveBytes + kMaxXmmArgs * kXmmSlotBytes;

		static constexpr U8 kSseOp[] = {0x58, 0x5c, 0x59, 0x5e}; // SSE arith opcodes: add sub mul div
		static constexpr U8 kIntCc[] = {CC_E, CC_NE, CC_L, CC_LE, CC_B, CC_BE};

		explicit X86EmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;

		static B32 isFloatTy(const Type* t);
		static B32 isX87Ty(const Type* t);
		static String libcName(const String& callee);
		static U32 intBits(const Type* t);
		static U32 opWidth(const Type* t);
	private:
		struct FunctionEmitter {
			const Function& fn;
			const Module& mod;
			Schedule sched;

			List<U8> code;				 // emitted machine code bytes
			List<AsmReloc> relocs; // relocations into code
			Asm a;								 // assembler writing into code/relocs

			Map<const Node*, I32> slot;			// value -> RBP-relative stack slot
			Map<const Node*, I32> allocOff; // fixed alloc -> RBP-relative storage offset
			U32 frameSize = 0;							// total frame size (16-byte aligned)
			I32 ldScratch = 0;							// scratch slot for loads/conversions

			B32 variadic = false;					// function takes varargs
			I32 saveArea = 0;							// register save area offset (variadic)
			U32 namedGp = 0, namedFp = 0; // named GP/FP args consumed before "..."
			I32 overflowOff = 16;					// first stack-passed arg offset (variadic)

			List<U32> blockOffset; // block index -> byte offset in code

			struct JumpFix {
				U32 dispAt;			 // offset of the rel32 displacement in code
				I32 targetBlock; // block the jump targets
			};

			List<JumpFix> fixes;

			using TK = Schedule::TermKind;

			explicit FunctionEmitter(const Function& f);

			I32 slotOf(const Node* n);		 // stack slot of a value (0 if none)
			void layout();								 // assign stack slots, compute frame size
			void layoutVariadic(U32& off); // reserve the register save area

			void loadInt(Node* n, Reg r);		// materialize integer/pointer into r
			void loadFloat(Node* n, U32 x); // load float into XMM x
			void storeInt(Node* n, Reg r);	// spill r to n's slot
			void storeFloat(Node* n, U32 x);
			void fldX87(Node* n);			 // push value onto x87 stack
			void fstpX87Slot(Node* n); // pop x87 top into n's slot

			void run();												// emit the whole function
			void prologue();									// frame setup + parameter spilling
			B32 wantsSlot(ProjNode* p) const; // parameter projection has a slot
			void emitParamLoad(ProjNode* p, Type* t, U32& intIdx, U32& xmmIdx, I32& stackOff);

			void emitNode(Node* n);
			void emitAlloc(AllocNode* al);
			void emitStore(StoreNode* s);
			void emitLoad(LoadNode* l);
			void emitBinary(BinaryNode* n);
			void emitFloatBinary(BinaryNode* n);
			void emitUnary(UnaryNode* n);
			void maskToBits(U32 bits, Reg r0, Reg r1 = RAX); // truncate to width
			void storeBool(Node* n, U8 cc);									 // setcc -> n's slot
			void emitCompare(CompareNode* n);
			void emitFloatCompare(CompareNode* n);
			void emitConvertX87(ConvertNode* n, Node* src, Opcode op); // x87 conversions
			void emitConvert(ConvertNode* n);

			// calls and the variadic ABI
			void emitVaStart(CallNode* c);
			void vaOverflow(I32 step);															// advance overflow_arg_area
			void vaRegOrStack(I32 offDisp, U32 limit, I32 regStep); // va_arg dispatch
			void emitVaArg(CallNode* c);
			void emitCall(CallNode* c);

			// control flow
			void emitPhiCopies(I32 targetBlock, I32 predIdx); // resolve phis on an edge
			void recordFix(U32 dispAt, I32 targetBlock);			// queue a jump to patch
			void jumpTo(I32 target, I32 fallthrough);					// jmp unless fallthrough
			void emitTerminator(I32 b, I32 fallthrough);
		};

		void emitModule(const Module& module); // emit globals then functions
		void emitGlobal(ElfObject& elf, const Module& mod, const Global* g); // lay out one global
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
