#include "IR/Opcode.h"

namespace rat {
	namespace detail {
		static const OpcodeInfo kOpcodeInfo[] = {
			// mnemonic    cfg  side  comm  ctrl  min  max
			{"start",      1,   0,    0,   -1,    0,   0},
			{"stop",       1,   1,    0,   -1,    0,  -1},
			{"return",     1,   1,    0,    0,    2,   3},
			{"region",     1,   0,    0,   -1,    1,  -1},
			{"if",         1,   0,    0,    0,    2,   2},
			{"proj",       0,   0,    0,   -1,    1,   1},
			{"phi",        0,   0,    0,    0,    1,  -1},
			{"const",      0,   0,    0,   -1,    0,   0},
			{"add",        0,   0,    1,   -1,    2,   2},
			{"sub",        0,   0,    0,   -1,    2,   2},
			{"mul",        0,   0,    1,   -1,    2,   2},
			{"sdiv",       0,   0,    0,   -1,    2,   2},
			{"udiv",       0,   0,    0,   -1,    2,   2},
			{"srem",       0,   0,    0,   -1,    2,   2},
			{"urem",       0,   0,    0,   -1,    2,   2},
			{"and",        0,   0,    1,   -1,    2,   2},
			{"or",         0,   0,    1,   -1,    2,   2},
			{"xor",        0,   0,    1,   -1,    2,   2},
			{"shl",        0,   0,    0,   -1,    2,   2},
			{"lshr",       0,   0,    0,   -1,    2,   2},
			{"ashr",       0,   0,    0,   -1,    2,   2},
			{"neg",        0,   0,    0,   -1,    1,   1},
			{"not",        0,   0,    0,   -1,    1,   1},
			{"eq",         0,   0,    1,   -1,    2,   2},
			{"ne",         0,   0,    1,   -1,    2,   2},
			{"slt",        0,   0,    0,   -1,    2,   2},
			{"sle",        0,   0,    0,   -1,    2,   2},
			{"ult",        0,   0,    0,   -1,    2,   2},
			{"ule",        0,   0,    0,   -1,    2,   2},
			{"trunc",      0,   0,    0,   -1,    1,   1},
			{"sext",       0,   0,    0,   -1,    1,   1},
			{"zext",       0,   0,    0,   -1,    1,   1},
			{"load",       0,   0,    0,    0,    3,   3},
			{"store",      0,   1,    0,    0,    4,   4},
			{"call",       0,   1,    0,    0,    2,  -1},
			{"global",     0,   0,    0,   -1,    0,   0},
			{"alloc",      0,   0,    0,   -1,    0,   0},
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
