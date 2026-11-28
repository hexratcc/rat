// expect: 4095
// host-side:
// stack aggregates aligned past 16 sit at their own alignment, the caller aligns rsp to match
#ifdef RAT_HOST_SIDE
#define SIDE(x) g_##x
#define OTHER(x) r_##x
#else
#define SIDE(x) r_##x
#define OTHER(x) g_##x
#endif
#include <stdarg.h>

typedef struct {
	_Alignas(32) int v;
} A32;

typedef struct {
	_Alignas(64) int v;
} A64;

int OTHER(named)(long a, long b, long c, long d, long e, long f, A32 x, long g, A64 y);
int OTHER(va)(int n, ...);

int SIDE(named)(long a, long b, long c, long d, long e, long f, A32 x, long g, A64 y) {
	return a + b + c + d + e + f == 15 && x.v == 1 && g == 2 && y.v == 3;
}

// n longs, then an A32, a long and an A64
int SIDE(va)(int n, ...) {
	va_list ap;
	va_start(ap, n);
	int ok = 1;
	for(int i = 0; i < n; ++i)
		ok &= va_arg(ap, long) == i;
	A32 x = va_arg(ap, A32);
	long g = va_arg(ap, long);
	A64 y = va_arg(ap, A64);
	va_end(ap);
	return ok && x.v == 1 && g == 2 && y.v == 3;
}

// shift moves rsp by 16, so a caller that does not realign misses on one of the two
int SIDE(calls)(int shift) {
	if(shift) {
		volatile char* pad = __builtin_alloca(16);
		pad[0] = 0;
	}
	A32 x = {1};
	A64 y = {3};
	int bits = 0;
	if(OTHER(named)(0, 1, 2, 3, 4, 5, x, 2, y))
		bits |= 1;
	// the A32 is the first stack arg
	if(OTHER(va)(0, x, 2L, y))
		bits |= 2;
	// a long on the stack pads the A32 to 32
	if(OTHER(va)(6, 0L, 1L, 2L, 3L, 4L, 5L, x, 2L, y))
		bits |= 4;
	return bits;
}

#ifndef RAT_HOST_SIDE
int g_calls(int shift);

int main(void) { return r_calls(0) | r_calls(1) << 3 | g_calls(0) << 6 | g_calls(1) << 9; }
#endif
