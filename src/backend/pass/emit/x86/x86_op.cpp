#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       cls   defs uses flags     imm             imm2
		// pseudo / data movement
		{"copy",         kGp,    1,   1, kOpCopy,  ImmKind::None,  ImmKind::None},
		{"loadimm",      kGp,    1,   1, kOpRemat, ImmKind::None,  ImmKind::None},
		{"loadsym",      kGp,    1,   1, kOpRemat, ImmKind::None,  ImmKind::None},
		{"frameaddr",    kGp,    1,   0, kOpRemat, ImmKind::Disp,  ImmKind::None},
		{"retaddr",      kGp,    1,   0, 0,        ImmKind::None,  ImmKind::None},
		{"lea",          kGp,    1,   2, 0,        ImmKind::Disp,  ImmKind::Other},
		// dynamic stack
		{"stackalloc",   kGp,    1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"stacksave",    kGp,    1,   0, 0,        ImmKind::None,  ImmKind::None},
		{"stackrestore", kGp,    0,   1, 0,        ImmKind::None,  ImmKind::None},
		// non-local goto
		{"setjmp",       kGp,    1,   1, kOpCall,  ImmKind::None,  ImmKind::None},
		{"longjmp",      kGp,    0,   1, 0,        ImmKind::None,  ImmKind::None},
		// integer memory
		{"load",         kGp,    1,  -1, kOpMem,   ImmKind::Disp,  ImmKind::Sib},
		{"store",        kGp,    0,  -1, kOpMem,   ImmKind::Disp,  ImmKind::Sib},
		// integer ALU
		{"add",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"sub",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"mul",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"and",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"or",           kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"xor",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"neg",          kGp,    1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"not",          kGp,    1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"shl",          kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"ashr",         kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"lshr",         kGp,    1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"rotl",         kGp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"rotr",         kGp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"sdiv",         kGp,    2,   2, 0,        ImmKind::Width, ImmKind::None},
		{"srem",         kGp,    2,   2, 0,        ImmKind::Width, ImmKind::None},
		{"udiv",         kGp,    2,   2, 0,        ImmKind::Width, ImmKind::None},
		{"urem",         kGp,    2,   2, 0,        ImmKind::Width, ImmKind::None},
		{"bsf",          kGp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"bsr",          kGp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"cmp",          kGp,    0,   2, 0,        ImmKind::None,  ImmKind::None},
		{"setcc",        kGp,    1,   0, 0,        ImmKind::Cc,    ImmKind::None},
		{"cmov",         kGp,    1,   2, 0,        ImmKind::Cc,    ImmKind::None},
		{"maskbits",     kGp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"signextbits",  kGp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"bswap",        kGp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		// sse scalar float
		{"fload",        kFp,    1,  -1, kOpMem,   ImmKind::Disp,  ImmKind::Sib},
		{"fstore",       kFp,    0,  -1, kOpMem,   ImmKind::Disp,  ImmKind::Sib},
		{"fadd",         kFp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"fsub",         kFp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"fmul",         kFp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"fdiv",         kFp,    1,   2, 0,        ImmKind::Width, ImmKind::None},
		{"fneg",         kFp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"fsqrt",        kFp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"fabs",         kFp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"fcmp",         kGp,    1,   2, 0,        ImmKind::Cc,    ImmKind::Other},
		{"fcmpflags",    kFp,    0,   2, 0,        ImmKind::None,  ImmKind::Other},
		{"cvt",          kFp,    1,   1, 0,        ImmKind::Cvt,   ImmKind::None},
		// sse packed vector
		{"varith",       kFp,    1,   2, 0,        ImmKind::Other, ImmKind::None},
		{"vsplat",       kFp,    1,   1, 0,        ImmKind::Lane,  ImmKind::Other},
		{"vextract",     kFp,    1,   1, 0,        ImmKind::Lane,  ImmKind::Other},
		{"vpack",        kFp,    1,  -1, 0,        ImmKind::Lane,  ImmKind::Other},
		{"vpackreg",     kFp,    1,  -1, 0,        ImmKind::Lane,  ImmKind::Other},
		{"vshuf",        kFp,    1,   1, 0,        ImmKind::Lane,  ImmKind::None},
		// x87
		// imm is the memory width, or -1 / -2 to store-and-pop or discard st(0)
		{"x87loadmem",   kX87,  -1,  -1, 0,        ImmKind::Other, ImmKind::None},
		{"x87storemem",  kX87,  -1,  -1, 0,        ImmKind::Other, ImmKind::None},
		{"x87loadimmd",  kX87,   1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"x87fromint",   kX87,   1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"x87toint",     kGp,    1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"x87fromsse",   kX87,   1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"x87tosse",     kFp,    1,   1, 0,        ImmKind::Width, ImmKind::None},
		{"x87add",       kX87,   1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"x87sub",       kX87,   1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"x87mul",       kX87,   1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"x87div",       kX87,   1,   2, 0,        ImmKind::None,  ImmKind::None},
		{"x87neg",       kX87,   1,   1, 0,        ImmKind::None,  ImmKind::None},
		{"x87cmp",       kGp,    1,   2, 0,        ImmKind::Cc,    ImmKind::Other},
		// control / calls
		{"call",         kGp,   -1,  -1, kOpCall,  ImmKind::Other, ImmKind::Other},
		{"ret",          kGp,    0,  -1, kOpTerm,  ImmKind::None,  ImmKind::None},
		{"jmp",          kGp,    0,   1, kOpTerm,  ImmKind::None,  ImmKind::None},
		{"switchjump",   kGp,    0,  -1, kOpTerm,  ImmKind::None,  ImmKind::None},
		{"br",           kGp,    0,  -1, kOpTerm,  ImmKind::Cc,    ImmKind::Other},
		// variadic support
		{"vastart",      kGp,    0,   1, 0,        ImmKind::Other, ImmKind::Other},
		{"vaarg",        kGp,    1,   1, 0,        ImmKind::Other, ImmKind::Other},
		// misc
		{"ud2",          kGp,    0,   0, 0,        ImmKind::None,  ImmKind::None},
		{"prefetch",     kGp,    0,   1, kOpMem,   ImmKind::Disp,  ImmKind::Other},
		};
		// clang-format on

		static_assert(sizeof(kX86OpInfo) / sizeof(kX86OpInfo[0]) == (U32)X86Op::Prefetch + 1,
									"kX86OpInfo must cover every X86Op");
	} // namespace detail

	const X86OpInfo& x86OpInfo(X86Op op) { return detail::kX86OpInfo[(U32)op]; }
	const C8* x86OpMnemonic(X86Op op) { return x86OpInfo(op).mnemonic; }

} // namespace rat
