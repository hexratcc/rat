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
	};

	const char* getOpcodeMnemonic(Opcode op);
} // namespace rat

#endif
