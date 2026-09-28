#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       cls   flags
		// pseudo / data movement
		{"copy",         kGp,  kOpCopy},
		{"loadimm",      kGp,  kOpRemat},
		{"loadsym",      kGp,  kOpRemat},
		{"frameaddr",    kGp,  kOpRemat},
		{"retaddr",      kGp,  0},
		{"lea",          kGp,  0},
		// dynamic stack
		{"stackalloc",   kGp,  0},
		{"stacksave",    kGp,  0},
		{"stackrestore", kGp,  0},
		// non-local goto
		{"setjmp",       kGp,  0},
		{"longjmp",      kGp,  0},
		// integer memory
		{"load",         kGp,  0},
		{"store",        kGp,  0},
		// integer ALU
		{"add",          kGp,  0},
		{"sub",          kGp,  0},
		{"mul",          kGp,  0},
		{"and",          kGp,  0},
		{"or",           kGp,  0},
		{"xor",          kGp,  0},
		{"neg",          kGp,  0},
		{"not",          kGp,  0},
		{"shl",          kGp,  0},
		{"ashr",         kGp,  0},
		{"lshr",         kGp,  0},
		{"rotl",         kGp,  0},
		{"rotr",         kGp,  0},
		{"sdiv",         kGp,  0},
		{"srem",         kGp,  0},
		{"udiv",         kGp,  0},
		{"urem",         kGp,  0},
		{"bsf",          kGp,  0},
		{"bsr",          kGp,  0},
		{"cmp",          kGp,  0},
		{"setcc",        kGp,  0},
		{"cmov",         kGp,  0},
		{"maskbits",     kGp,  0},
		{"signextbits",  kGp,  0},
		{"bswap",        kGp,  0},
		// sse scalar float
		{"fload",        kFp,  0},
		{"fstore",       kFp,  0},
		{"fadd",         kFp,  0},
		{"fsub",         kFp,  0},
		{"fmul",         kFp,  0},
		{"fdiv",         kFp,  0},
		{"fneg",         kFp,  0},
		{"fsqrt",        kFp,  0},
		{"fabs",         kFp,  0},
		{"fcmp",         kGp,  0},
		{"fcmpflags",    kFp,  0},
		{"cvt",          kFp,  0},
		// sse packed vector
		{"varith",       kFp,  0},
		{"vsplat",       kFp,  0},
		{"vextract",     kFp,  0},
		{"vpacklane",    kFp,  0},
		{"vpack",        kFp,  0},
		{"vpackreg",     kFp,  0},
		{"vinsertreg",   kFp,  0},
		{"vshuf",        kFp,  0},
		// x87
		{"x87loadmem",   kX87, 0},
		{"x87storemem",  kX87, 0},
		{"x87loadimmd",  kX87, 0},
		{"x87fromint",   kX87, 0},
		{"x87toint",     kGp,  0},
		{"x87fromsse",   kX87, 0},
		{"x87tosse",     kFp,  0},
		{"x87add",       kX87, 0},
		{"x87sub",       kX87, 0},
		{"x87mul",       kX87, 0},
		{"x87div",       kX87, 0},
		{"x87neg",       kX87, 0},
		{"x87cmp",       kGp,  0},
		// control / calls
		{"call",         kGp,  0},
		{"ret",          kGp,  0},
		{"jmp",          kGp,  0},
		{"switchjump",   kGp,  0},
		{"br",           kGp,  0},
		// variadic support
		{"vastart",      kGp,  0},
		{"vaarg",        kGp,  0},
		// misc
		{"ud2",          kGp,  0},
		{"prefetch",     kGp,  0},
		};
		// clang-format on

		static_assert(sizeof(kX86OpInfo) / sizeof(kX86OpInfo[0]) == (U32)X86Op::Prefetch + 1,
									"kX86OpInfo must cover every X86Op");
	} // namespace detail

	const X86OpInfo& x86OpInfo(X86Op op) { return detail::kX86OpInfo[(U32)op]; }
} // namespace rat
