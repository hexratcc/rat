#ifndef RAT_IR_OPCODE_H
#define RAT_IR_OPCODE_H

#include "Core.h"

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

		// unary
		Neg,
		Not,

		// comparisons (always produce bool)
		Eq,
		Ne,
		Slt,
		Sle,
		Ult,
		Ule,

		// conversions
		Trunc,
		SExt,
		ZExt,

		// memory
		Load,
		Store,

		// calls
		Call,

		// storage: a pointer to a module global or a stack allocation
		Global,
		Alloc,
	};

	enum class OpClass : U8 {
		None,
		Binary,
		Unary,
		Compare,
		Convert,
	};

	struct OpcodeInfo {
		const char* mnemonic;
		B32 isCFG;
		B32 hasSideEffects;
		B32 isCommutative;
		I8 controlInputIndex;
		I8 minInputs;
		I8 maxInputs;
		OpClass opClass;
	};

	const OpcodeInfo& getOpcodeInfo(Opcode op);

	const char* getOpcodeMnemonic(Opcode op);

	inline OpClass getOpClass(Opcode op) { return getOpcodeInfo(op).opClass; }

	constexpr B32 isBinaryOpcode(Opcode op) {
		return op >= Opcode::Add && op <= Opcode::AShr;
	}
	constexpr B32 isUnaryOpcode(Opcode op) {
		return op >= Opcode::Neg && op <= Opcode::Not;
	}
	constexpr B32 isCompareOpcode(Opcode op) {
		return op >= Opcode::Eq && op <= Opcode::Ule;
	}
	constexpr B32 isConvertOpcode(Opcode op) {
		return op >= Opcode::Trunc && op <= Opcode::ZExt;
	}
} // namespace rat

#endif
