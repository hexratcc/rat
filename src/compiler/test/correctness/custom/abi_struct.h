// aggregates of each x86-64 eightbyte class by value, as arguments, returns and through ...
// SIDE(x) names this side's callees, OTHER(x) the other side's
#include <stdarg.h>

typedef struct {
	float x, y;
} V2; // sse
typedef struct {
	float x, y, z, w;
} V4; // sse sse
typedef struct {
	long a;
	double b;
} LD; // int sse
typedef struct {
	double a;
	long b;
} DL; // sse int
typedef struct {
	long a, b;
} L2; // int int
typedef struct {
	char c[3];
} C3; // int
typedef struct {
	int i;
	float f;
} IF; // int
typedef union {
	float f;
	int i;
} UFI; // int
typedef struct {
	struct {
		float v[3];
	} in;
	int n;
} NA; // sse int
typedef struct {
	unsigned a : 3, b : 20, c : 9;
} BF; // int
typedef struct {
	_Alignas(16) long a;
	long b;
} A16; // int int, 16 aligned
typedef struct {
	long a, b, c;
} L3; // memory
typedef struct {
	_Alignas(16) long a;
	long b, c;
} M16; // memory, 16 aligned
typedef struct {
	long double x;
} LDX; // memory, returned in st0
typedef struct {
	long v[9];
} L9; // memory, copied as one block
typedef union {
	long double x;
	long l[2];
} ULX; // int int
typedef struct {
} E; // nothing

#define MIX(h, v) ((h) * 1000003u + (unsigned long)(v))
#define MIXF(h, v) MIX(h, (long)((v) * 4))
#define MISALIGNED(p) (((unsigned long)(p) & 15) != 0)

static unsigned long sumV2(unsigned long h, const V2* s) { return MIXF(MIXF(h, s->x), s->y); }
static unsigned long sumV4(unsigned long h, const V4* s) {
	return MIXF(MIXF(MIXF(MIXF(h, s->x), s->y), s->z), s->w);
}
static unsigned long sumLD(unsigned long h, const LD* s) { return MIXF(MIX(h, s->a), s->b); }
static unsigned long sumDL(unsigned long h, const DL* s) { return MIX(MIXF(h, s->a), s->b); }
static unsigned long sumL2(unsigned long h, const L2* s) { return MIX(MIX(h, s->a), s->b); }
static unsigned long sumC3(unsigned long h, const C3* s) {
	return MIX(MIX(MIX(h, s->c[0]), s->c[1]), s->c[2]);
}
static unsigned long sumIF(unsigned long h, const IF* s) { return MIXF(MIX(h, s->i), s->f); }
static unsigned long sumNA(unsigned long h, const NA* s) {
	return MIX(MIXF(MIXF(MIXF(h, s->in.v[0]), s->in.v[1]), s->in.v[2]), s->n);
}
static unsigned long sumBF(unsigned long h, const BF* s) {
	return MIX(MIX(MIX(h, s->a), s->b), s->c);
}
static unsigned long sumA16(unsigned long h, const A16* s) {
	return MIX(MIX(MIX(h, s->a), s->b), MISALIGNED(s));
}
static unsigned long sumL3(unsigned long h, const L3* s) {
	return MIX(MIX(MIX(h, s->a), s->b), s->c);
}
static unsigned long sumM16(unsigned long h, const M16* s) {
	return MIX(MIX(MIX(MIX(h, s->a), s->b), s->c), MISALIGNED(s));
}
static unsigned long sumLDX(unsigned long h, const LDX* s) { return MIXF(h, s->x); }
static unsigned long sumL9(unsigned long h, const L9* s) {
	for(int i = 0; i < 9; ++i)
		h = MIX(h, s->v[i]);
	return h;
}

static const V2 kV2a = {1.5f, -2.25f}, kV2b = {10, 11};
static const V4 kV4 = {4, 5.5f, -6, 7.75f};
static const LD kLD = {-123456789012L, 3.5};
static const DL kDL = {-0.75, 99};
static const L2 kL2 = {0x1122334455667788L, -42};
static const C3 kC3 = {{'r', 'a', 't'}};
static const IF kIF = {-7, 2.5f};
static const UFI kUFI = {.f = 1.25f};
static const NA kNA = {{{0.5f, 1.5f, 2.5f}}, 33};
static const BF kBF = {5, 0xabcde, 0x155};
static const A16 kA16 = {16, -16};
static const L3 kL3 = {1, -2, 3};
static const M16 kM16 = {0x7fffffffffffL, 5, -6};
static const LDX kLDX = {2.75L};
static const L9 kL9 = {{1, -2, 3, -4, 5, -6, 7, -8, 0x123456789L}};
static const E kE;

static int fold(unsigned long h) { return (int)(h ^ h >> 32); }

#define CALLEES(F)                                                                                 \
	unsigned long F(take)(V2 a, int n, V4 c, float lo, V2 b, int stride);                            \
	unsigned long F(mixed)(LD a, DL b, C3 c, IF d, UFI e, NA f, BF g);                               \
	unsigned long F(mem)(L3 a, LDX b, M16 c, int tail);                                              \
	unsigned long F(spillGp)(int a, int b, int c, int d, int e, L2 s, long g);                       \
	unsigned long F(spillSse)(                                                                       \
			double a, double b, double c, double d, double e, double f, double g, V4 s, double h);       \
	unsigned long F(spillMixed)(long a, long b, long c, long d, long e, long f, LD s, double t);     \
	unsigned long F(stackAlign)(int a, int b, int c, int d, int e, int f, long x, A16 s, long y);    \
	unsigned long F(emptyArg)(int a, E e, int b);                                                    \
	unsigned long F(complex)(float _Complex a, double _Complex b, int n);                            \
	unsigned long F(big)(int a, L9 s, L9 t, long b);                                                 \
	unsigned long F(target)(LD a, V4 b, V2 c);                                                       \
	V2 F(v2)(float x);                                                                               \
	V4 F(v4)(float x);                                                                               \
	LD F(ld)(long a, double b);                                                                      \
	DL F(dl)(double a, long b);                                                                      \
	C3 F(c3)(char c);                                                                                \
	IF F(ifs)(int i);                                                                                \
	NA F(na)(int n);                                                                                 \
	A16 F(a16)(long a);                                                                              \
	L3 F(l3)(long a, long b, long c, long d, long e, long f);                                        \
	LD F(ldMany)(long a, long b, long c, long d, long e, long f);                                    \
	E F(emptyRet)(int* hit);                                                                         \
	double _Complex F(cd)(double x);                                                                 \
	LDX F(ldx)(long double x);                                                                       \
	ULX F(ulx)(long a);                                                                              \
	long double _Complex F(cld)(long double x);                                                      \
	float _Complex F(cf)(float x);                                                                   \
	unsigned long F(va)(const char* fmt, ...);                                                       \
	LD F(vaLd)(const char* fmt, ...);                                                                \
	L3 F(vaL3)(const char* fmt, ...);                                                                \
	unsigned long F(calls)(void);
CALLEES(SIDE)
CALLEES(OTHER)

unsigned long SIDE(take)(V2 a, int n, V4 c, float lo, V2 b, int stride) {
	return MIX(sumV2(MIXF(sumV4(MIX(sumV2(1, &a), n), &c), lo), &b), stride);
}

unsigned long SIDE(mixed)(LD a, DL b, C3 c, IF d, UFI e, NA f, BF g) {
	unsigned long h = sumDL(sumLD(2, &a), &b);
	h = MIXF(sumIF(sumC3(h, &c), &d), e.f);
	return sumBF(sumNA(h, &f), &g);
}

unsigned long SIDE(mem)(L3 a, LDX b, M16 c, int tail) {
	a.b = 100; // the callee owns its copy
	return MIX(sumM16(sumLDX(sumL3(3, &a), &b), &c), tail);
}

unsigned long SIDE(spillGp)(int a, int b, int c, int d, int e, L2 s, long g) {
	return MIX(sumL2(MIX(MIX(MIX(MIX(MIX(4, a), b), c), d), e), &s), g);
}

unsigned long SIDE(spillSse)(
		double a, double b, double c, double d, double e, double f, double g, V4 s, double h) {
	unsigned long r = MIXF(MIXF(MIXF(MIXF(MIXF(MIXF(MIXF(5, a), b), c), d), e), f), g);
	return MIXF(sumV4(r, &s), h);
}

unsigned long SIDE(spillMixed)(long a, long b, long c, long d, long e, long f, LD s, double t) {
	return MIXF(sumLD(MIX(MIX(MIX(MIX(MIX(MIX(6, a), b), c), d), e), f), &s), t);
}

unsigned long SIDE(stackAlign)(int a, int b, int c, int d, int e, int f, long x, A16 s, long y) {
	unsigned long h = MIX(MIX(MIX(MIX(MIX(MIX(7, a), b), c), d), e), f);
	return MIX(sumA16(MIX(h, x), &s), y);
}

unsigned long SIDE(emptyArg)(int a, E e, int b) { return MIX(MIX(8, a), b); }

unsigned long SIDE(complex)(float _Complex a, double _Complex b, int n) {
	return MIX(MIXF(MIXF(MIXF(MIXF(9, __real__ a), __imag__ a), __real__ b), __imag__ b), n);
}

unsigned long SIDE(target)(LD a, V4 b, V2 c) { return sumV2(sumV4(sumLD(10, &a), &b), &c); }

unsigned long SIDE(big)(int a, L9 s, L9 t, long b) {
	s.v[0] = 50; // the callee owns its copy
	return MIX(sumL9(sumL9(MIX(11, a), &s), &t), b);
}

V2 SIDE(v2)(float x) { return (V2){x, x + 1}; }
V4 SIDE(v4)(float x) { return (V4){x, -x, x * 2, 0.5f}; }
LD SIDE(ld)(long a, double b) { return (LD){a, b}; }
DL SIDE(dl)(double a, long b) { return (DL){a, b}; }
C3 SIDE(c3)(char c) { return (C3){{c, (char)(c + 1), (char)(c + 2)}}; }
IF SIDE(ifs)(int i) { return (IF){i, (float)i / 2}; }
NA SIDE(na)(int n) { return (NA){{{0.25f, 0.5f, 0.75f}}, n}; }
A16 SIDE(a16)(long a) { return (A16){a, ~a}; }
L3 SIDE(l3)(long a, long b, long c, long d, long e, long f) { return (L3){a + b, c + d, e + f}; }
LD SIDE(ldMany)(long a, long b, long c, long d, long e, long f) {
	return (LD){a * b * c, (double)(d + e + f)};
}

E SIDE(emptyRet)(int* hit) {
	*hit += 1;
	return kE;
}

double _Complex SIDE(cd)(double x) { return x - 2 * x * 1.0i; }
float _Complex SIDE(cf)(float x) { return x + 3 * x * 1.0fi; }
LDX SIDE(ldx)(long double x) { return (LDX){x * 3}; }
long double _Complex SIDE(cld)(long double x) { return x + 2 * x * 1.0i; }
ULX SIDE(ulx)(long a) { return (ULX){.l = {a, ~a}}; }

// one checksum step per format letter
static unsigned long SIDE(next)(unsigned long h, char kind, va_list* ap) {
	switch(kind) {
	case 'l':
		return MIX(h, va_arg(*ap, long));
	case 'd':
		return MIXF(h, va_arg(*ap, double));
	case 'v': {
		V2 s = va_arg(*ap, V2);
		return sumV2(h, &s);
	}
	case 'V': {
		V4 s = va_arg(*ap, V4);
		return sumV4(h, &s);
	}
	case 'L': {
		LD s = va_arg(*ap, LD);
		return sumLD(h, &s);
	}
	case 'P': {
		L2 s = va_arg(*ap, L2);
		return sumL2(h, &s);
	}
	case 'c': {
		C3 s = va_arg(*ap, C3);
		return sumC3(h, &s);
	}
	case 'n': {
		NA s = va_arg(*ap, NA);
		return sumNA(h, &s);
	}
	case 'A': {
		A16 s = va_arg(*ap, A16);
		return sumA16(h, &s) - MISALIGNED(&s);
	}
	case 'M': {
		L3 s = va_arg(*ap, L3);
		return sumL3(h, &s);
	}
	case 'Q': {
		M16 s = va_arg(*ap, M16);
		return sumM16(h, &s) - MISALIGNED(&s);
	}
	case 'X': {
		LDX s = va_arg(*ap, LDX);
		return sumLDX(h, &s);
	}
	case 'z': {
		double _Complex z = va_arg(*ap, double _Complex);
		return MIXF(MIXF(h, __real__ z), __imag__ z);
	}
	case 'f': {
		float _Complex z = va_arg(*ap, float _Complex);
		return MIXF(MIXF(h, __real__ z), __imag__ z);
	}
	case 'B': {
		L9 s = va_arg(*ap, L9);
		return sumL9(h, &s);
	}
	}
	return MIX(h, 999);
}

static unsigned long SIDE(vaFold)(unsigned long h, const char* fmt, va_list* ap) {
	for(const char* p = fmt; *p; ++p)
		h = SIDE(next)(h, *p, ap);
	return h;
}

unsigned long SIDE(va)(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	unsigned long h = SIDE(vaFold)(1, fmt, &ap);
	va_end(ap);
	return h;
}

// returns in registers and through a hidden pointer shift the named registers differently
LD SIDE(vaLd)(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	unsigned long h = SIDE(vaFold)(2, fmt, &ap);
	va_end(ap);
	return (LD){(long)h, 0.5};
}

L3 SIDE(vaL3)(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	unsigned long h = SIDE(vaFold)(3, fmt, &ap);
	va_end(ap);
	return (L3){(long)h, 4, 5};
}

static unsigned long SIDE(argCalls)(void) {
	L3 l3 = kL3;
	unsigned long h = OTHER(take)(kV2a, 3, kV4, 9, kV2b, 4);
	h = MIX(h, OTHER(mixed)(kLD, kDL, kC3, kIF, kUFI, kNA, kBF));
	h = MIX(h, OTHER(mem)(l3, kLDX, kM16, 5));
	h = MIX(h, l3.b);
	h = MIX(h, OTHER(spillGp)(1, 2, 3, 4, 5, kL2, 6));
	h = MIX(h, OTHER(spillSse)(1, 2, 3, 4, 5, 6, 7, kV4, 8.5));
	h = MIX(h, OTHER(spillMixed)(1, 2, 3, 4, 5, 6, kLD, -1.25));
	h = MIX(h, OTHER(stackAlign)(1, 2, 3, 4, 5, 6, 70, kA16, 80));
	h = MIX(h, OTHER(emptyArg)(11, kE, 12));
	h = MIX(h, OTHER(complex)(1.5f + 2.5fi, -3.25 + 4.75i, 13));
	L9 l9 = kL9;
	h = MIX(h, OTHER(big)(1, l9, kL9, 2));
	h = MIX(h, l9.v[0]);
	unsigned long (*volatile target)(LD, V4, V2) = OTHER(target);
	return MIX(h, target(kLD, kV4, kV2b));
}

static unsigned long SIDE(retCalls)(void) {
	V2 v2 = OTHER(v2)(1.5f);
	V4 v4 = OTHER(v4)(2.5f);
	LD ld = OTHER(ld)(-77, 4.5);
	DL dl = OTHER(dl)(-5.75, 66);
	C3 c3 = OTHER(c3)('k');
	IF ifs = OTHER(ifs)(-9);
	NA na = OTHER(na)(55);
	A16 a16 = OTHER(a16)(0x123456789L);
	L3 l3 = OTHER(l3)(1, 2, 3, 4, 5, 6);
	LD ldMany = OTHER(ldMany)(1, 2, 3, 4, 5, 6);
	int hit = 0;
	OTHER(emptyRet)(&hit);
	double _Complex cd = OTHER(cd)(1.25);
	float _Complex cf = OTHER(cf)(-0.5f);
	LDX ldx = OTHER(ldx)(1.25L);
	long double _Complex cld = OTHER(cld)(-0.75L);
	ULX ulx = OTHER(ulx)(0x55aa);
	OTHER(ldx)(9.5L); // the discarded st0 is popped
	OTHER(cld)(9.5L);
	LD (*volatile ldFn)(long, double) = OTHER(ld);
	LD ldIndirect = ldFn(88, -0.25);

	unsigned long h = sumV4(sumV2(1, &v2), &v4);
	h = sumDL(sumLD(h, &ld), &dl);
	h = sumNA(sumIF(sumC3(h, &c3), &ifs), &na);
	h = sumLD(sumL3(sumA16(h, &a16), &l3), &ldMany);
	h = MIXF(MIXF(MIX(h, hit), __real__ cd), __imag__ cd);
	h = MIXF(MIXF(h, __real__ cf), __imag__ cf);
	h = MIXF(MIXF(sumLDX(h, &ldx), __real__ cld), __imag__ cld);
	h = MIX(MIX(h, ulx.l[0]), ulx.l[1]);
	return sumLD(h, &ldIndirect);
}

static unsigned long SIDE(vaCalls)(void) {
	unsigned long h = OTHER(va)(
			"vLVPcnAMQXzl", kV2a, kLD, kV4, kL2, kC3, kNA, kA16, kL3, kM16, kLDX, 1.5 - 2.5i, 77L);
	// the third L2 finds one gp register left and overflows, the long after it still gets r9
	h = MIX(h, OTHER(va)("PPPl", kL2, kL2, kL2, 5L));
	// four V4 fill the sse registers, the fifth and the double overflow
	h = MIX(h, OTHER(va)("VVVVVd", kV4, kV4, kV4, kV4, kV4, 1.5));
	// five LD take the gp registers left, the rest overflow while the double gets xmm5
	h = MIX(h, OTHER(va)("LLLLLLLd", kLD, kLD, kLD, kLD, kLD, kLD, kLD, 2.5));
	// a V2 after eight doubles overflows, the A16 after it still fits in gp registers
	h = MIX(h, OTHER(va)("ddddddddvAl", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, kV2b, kA16, 9L));
	// a float _Complex is not promoted, a large aggregate overflows as one block
	h = MIX(h, OTHER(va)("fBfl", 0.5f + 0.75fi, kL9, -1.5f + 2.5fi, 3L));
	// sixteen-byte aligned overflow slots
	h = MIX(h, OTHER(va)("lllllAlQX", 1L, 2L, 3L, 4L, 5L, kA16, 6L, kM16, kLDX));
	LD ld = OTHER(vaLd)("PPPlVL", kL2, kL2, kL2, 6L, kV4, kLD);
	L3 l3 = OTHER(vaL3)("PPlPdv", kL2, kL2, 7L, kL2, 0.25, kV2a);
	return sumL3(sumLD(h, &ld), &l3);
}

unsigned long SIDE(calls)(void) { return MIX(MIX(SIDE(argCalls)(), SIDE(retCalls)()), SIDE(vaCalls)()); }
