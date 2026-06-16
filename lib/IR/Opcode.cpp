#include "IR/Opcode.h"

namespace rat {
	namespace detail {
		static const OpcodeInfo kOpcodeInfo[] = {
			// mnemonic    cfg  side  comm  ctrl
			{"start",      1,   0,    0,   -1},
			{"stop",       1,   1,    0,   -1},
			{"return",     1,   1,    0,    0},
			{"region",     1,   0,    0,   -1},
			{"if",         1,   0,    0,    0},
			{"proj",       0,   0,    0,   -1},
			{"phi",        0,   0,    0,    0},
			{"const",      0,   0,    0,   -1},
			{"add",        0,   0,    1,   -1},
			{"sub",        0,   0,    0,   -1},
			{"mul",        0,   0,    1,   -1},
			{"sdiv",       0,   0,    0,   -1},
			{"udiv",       0,   0,    0,   -1},
			{"srem",       0,   0,    0,   -1},
			{"urem",       0,   0,    0,   -1},
			{"and",        0,   0,    1,   -1},
			{"or",         0,   0,    1,   -1},
			{"xor",        0,   0,    1,   -1},
			{"shl",        0,   0,    0,   -1},
			{"lshr",       0,   0,    0,   -1},
			{"ashr",       0,   0,    0,   -1},
			{"neg",        0,   0,    0,   -1},
			{"not",        0,   0,    0,   -1},
			{"eq",         0,   0,    1,   -1},
			{"ne",         0,   0,    1,   -1},
			{"slt",        0,   0,    0,   -1},
			{"sle",        0,   0,    0,   -1},
			{"ult",        0,   0,    0,   -1},
			{"ule",        0,   0,    0,   -1},
			{"trunc",      0,   0,    0,   -1},
			{"sext",       0,   0,    0,   -1},
			{"zext",       0,   0,    0,   -1},
			{"load",       0,   0,    0,    0},
			{"store",      0,   1,    0,    0},
			{"call",       0,   1,    0,    0},
			{"global",     0,   0,    0,   -1},
			{"alloc",      0,   0,    0,   -1},
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
