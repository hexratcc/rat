#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       cls
		// pseudo / data movement
		{"copy",         kGp},
		{"loadimm",      kGp},
		{"loadsym",      kGp},
		{"frameaddr",    kGp},
		{"retaddr",      kGp},
		{"lea",          kGp},
		// dynamic stack
		{"stackalloc",   kGp},
		{"stacksave",    kGp},
		{"stackrestore", kGp},
		// non-local goto
		{"setjmp",       kGp},
		{"longjmp",      kGp},
		// integer memory
		{"load",         kGp},
		{"store",        kGp},
		// integer ALU
		{"add",          kGp},
		{"sub",          kGp},
		{"mul",          kGp},
		{"and",          kGp},
		{"or",           kGp},
		{"xor",          kGp},
		{"neg",          kGp},
		{"not",          kGp},
		{"shl",          kGp},
		{"ashr",         kGp},
		{"lshr",         kGp},
		{"rotl",         kGp},
		{"rotr",         kGp},
		{"sdiv",         kGp},
		{"srem",         kGp},
		{"udiv",         kGp},
		{"urem",         kGp},
		{"bsf",          kGp},
		{"bsr",          kGp},
		{"cmp",          kGp},
		{"setcc",        kGp},
		{"cmov",         kGp},
		{"maskbits",     kGp},
		{"signextbits",  kGp},
		{"bswap",        kGp},
		// sse scalar float
		{"fload",        kFp},
		{"fstore",       kFp},
		{"fadd",         kFp},
		{"fsub",         kFp},
		{"fmul",         kFp},
		{"fdiv",         kFp},
		{"fneg",         kFp},
		{"fsqrt",        kFp},
		{"fabs",         kFp},
		{"fcmp",         kGp},
		{"fcmpflags",    kFp},
		{"cvt",          kFp},
		// sse packed vector
		{"varith",       kFp},
		{"vsplat",       kFp},
		{"vextract",     kFp},
		{"vpacklane",    kFp},
		{"vpack",        kFp},
		{"vpackreg",     kFp},
		{"vinsertreg",   kFp},
		{"vshuf",        kFp},
		// x87
		{"x87loadmem",   kX87},
		{"x87storemem",  kX87},
		{"x87loadimmd",  kX87},
		{"x87fromint",   kX87},
		{"x87toint",     kGp},
		{"x87fromsse",   kX87},
		{"x87tosse",     kFp},
		{"x87add",       kX87},
		{"x87sub",       kX87},
		{"x87mul",       kX87},
		{"x87div",       kX87},
		{"x87neg",       kX87},
		{"x87cmp",       kGp},
		// control / calls
		{"call",         kGp},
		{"ret",          kGp},
		{"jmp",          kGp},
		{"switchjump",   kGp},
		{"br",           kGp},
		// variadic support
		{"vastart",      kGp},
		{"vaarg",        kGp},
		// misc
		{"ud2",          kGp},
		{"prefetch",     kGp},
		};
		// clang-format on

		static_assert(sizeof(kX86OpInfo) / sizeof(kX86OpInfo[0]) == (U32)X86Op::Prefetch + 1,
									"kX86OpInfo must cover every X86Op");
	} // namespace detail

	const X86OpInfo& x86OpInfo(X86Op op) { return detail::kX86OpInfo[(U32)op]; }
} // namespace rat
