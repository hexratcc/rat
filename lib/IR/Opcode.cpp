#include "IR/Opcode.h"

namespace rat {
	namespace detail {
		static const OpcodeInfo kOpcodeInfo[] = {
			// mnemonic    cfg  side  comm  ctrl  min  max  class
			{"start",      1,   0,    0,   -1,    0,   0,  OpClass::None},
			{"stop",       1,   1,    0,   -1,    0,  -1,  OpClass::None},
			{"return",     1,   1,    0,    0,    2,   3,  OpClass::None},
			{"region",     1,   0,    0,   -1,    1,  -1,  OpClass::None},
			{"if",         1,   0,    0,    0,    2,   2,  OpClass::None},
			{"proj",       0,   0,    0,   -1,    1,   1,  OpClass::None},
			{"phi",        0,   0,    0,    0,    1,  -1,  OpClass::None},
			{"const",      0,   0,    0,   -1,    0,   0,  OpClass::None},
			{"add",        0,   0,    1,   -1,    2,   2,  OpClass::Binary},
			{"sub",        0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"mul",        0,   0,    1,   -1,    2,   2,  OpClass::Binary},
			{"sdiv",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"udiv",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"srem",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"urem",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"and",        0,   0,    1,   -1,    2,   2,  OpClass::Binary},
			{"or",         0,   0,    1,   -1,    2,   2,  OpClass::Binary},
			{"xor",        0,   0,    1,   -1,    2,   2,  OpClass::Binary},
			{"shl",        0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"lshr",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"ashr",       0,   0,    0,   -1,    2,   2,  OpClass::Binary},
			{"neg",        0,   0,    0,   -1,    1,   1,  OpClass::Unary},
			{"not",        0,   0,    0,   -1,    1,   1,  OpClass::Unary},
			{"eq",         0,   0,    1,   -1,    2,   2,  OpClass::Compare},
			{"ne",         0,   0,    1,   -1,    2,   2,  OpClass::Compare},
			{"slt",        0,   0,    0,   -1,    2,   2,  OpClass::Compare},
			{"sle",        0,   0,    0,   -1,    2,   2,  OpClass::Compare},
			{"ult",        0,   0,    0,   -1,    2,   2,  OpClass::Compare},
			{"ule",        0,   0,    0,   -1,    2,   2,  OpClass::Compare},
			{"trunc",      0,   0,    0,   -1,    1,   1,  OpClass::Convert},
			{"sext",       0,   0,    0,   -1,    1,   1,  OpClass::Convert},
			{"zext",       0,   0,    0,   -1,    1,   1,  OpClass::Convert},
			{"load",       0,   0,    0,    0,    3,   3,  OpClass::None},
			{"store",      0,   1,    0,    0,    4,   4,  OpClass::None},
			{"call",       0,   1,    0,    0,    2,  -1,  OpClass::None},
			{"global",     0,   0,    0,   -1,    0,   0,  OpClass::None},
			{"alloc",      0,   0,    0,   -1,    0,   0,  OpClass::None},
		};
		// clang-format on
	} // namespace detail

	const OpcodeInfo& getOpcodeInfo(Opcode op) {
		return detail::kOpcodeInfo[(U32)op];
	}

	const char* getOpcodeMnemonic(Opcode op) {
		return getOpcodeInfo(op).mnemonic;
	}
} // namespace rat
