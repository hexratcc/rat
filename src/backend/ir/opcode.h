#ifndef RAT_IR_OPCODE_H
#define RAT_IR_OPCODE_H

#include "core.h"

namespace rat {
	enum class Opcode {
		// control / structural
		Start,
		Stop,
		Return,
		Region,
		If,
		Proj,
		Phi,

		// constants
		Constant,

		// binary arithmetic / logic
		Add,
		Sub,
		Mul,
		SDiv,
		UDiv,
		SRem,
		URem,
		And,
		Or,
		Xor,
		Shl,
		LShr,
		AShr,
		Rotl,
		Rotr,
		// floating-point arithmetic
		FAdd,
		FSub,
		FMul,
		FDiv,

		// unary
		Neg,
		Not,
		FNeg,

		// comparisons
		Eq,
		Ne,
		Slt,
		Sle,
		Ult,
		Ule,
		// floating-point comparisons
		FEq,
		FNe,
		FLt,
		FLe,
		FGt,
		FGe,

		// conversions
		Trunc,
		SExt,
		ZExt,
		// int<->float and float<->float conversions
		SIToFP,
		UIToFP,
		FPToSI,
		FPToUI,
		FPExt,
		FPTrunc,

		// memory
		Load,
		Store,

		// calls
		Call,

		// storage: a pointer to a module global or a stack allocation
		Global,
		Alloc,

		// multi-way branch: control + selector in, one control proj per slot;
		// selector must be in [0, slots) (emitter guards it)
		Switch,

		// vector lane ops
		Splat,
		Extract,
		Pack,
	};

	enum class OpClass : U8 {
		None,
		Binary,
		Unary,
		Compare,
		Convert,
	};

	struct OpcodeInfo {
		const C8* mnemonic;
		B32 isCFG;
		B32 hasSideEffects;
		B32 isCommutative;
		I8 controlInputIndex; // operand slot holding the control (or -1)
		I8 minInputs;
		I8 maxInputs; // -1 => variadic
		OpClass opClass;
	};

	const OpcodeInfo& getOpcodeInfo(Opcode op);
	const C8* getOpcodeMnemonic(Opcode op);
	OpClass getOpClass(Opcode op);

	constexpr B32 isBinaryOpcode(Opcode op) { return op >= Opcode::Add && op <= Opcode::FDiv; }
	constexpr B32 isUnaryOpcode(Opcode op) { return op >= Opcode::Neg && op <= Opcode::FNeg; }
	constexpr B32 isCompareOpcode(Opcode op) { return op >= Opcode::Eq && op <= Opcode::FGe; }
	constexpr B32 isConvertOpcode(Opcode op) { return op >= Opcode::Trunc && op <= Opcode::FPTrunc; }
	constexpr B32 isArithmeticOpcode(Opcode op) {
		return isBinaryOpcode(op) || isUnaryOpcode(op) || isCompareOpcode(op) || isConvertOpcode(op);
	}
	constexpr B32 isVectorUtilOpcode(Opcode op) {
		return op == Opcode::Splat || op == Opcode::Extract || op == Opcode::Pack;
	}
} // namespace rat

#endif
