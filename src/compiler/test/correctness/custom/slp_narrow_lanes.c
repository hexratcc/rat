// expect: 0
// Byte and word lanes pack. SSE has no byte shift or multiply, see emitByteShift and emitByteMul.
// Every byte value is tried, so 0x80 and carries are covered
#define L4(e, k) e(k) e(k + 1) e(k + 2) e(k + 3)
#define L16(e) L4(e, 0) L4(e, 4) L4(e, 8) L4(e, 12)
typedef signed char s8;
typedef unsigned char u8;

#define MUL8(i) d[i] = a[i] * b[i];
#define SHL8(i) d[i] = a[i] << 7;
#define LSHR8(i) d[i] = (u8)a[i] >> 3;
#define ASHR8(i) d[i] = a[i] >> 1;
#define SEXT_LSHR8(i) d[i] = (unsigned)a[i] >> 6;
#define MUL16(i) e[i] = x[i] * y[i];
#define ASHR16(i) e[i] = x[i] >> 15;
#define TRUNC16(i) e[i] = w[i];

__attribute__((noinline)) void mul8(s8* restrict d, const s8* restrict a, const s8* restrict b) {
	L16(MUL8)
}
__attribute__((noinline)) void shl8(s8* restrict d, const s8* restrict a) { L16(SHL8) }
__attribute__((noinline)) void lshr8(s8* restrict d, const s8* restrict a) { L16(LSHR8) }
__attribute__((noinline)) void ashr8(s8* restrict d, const s8* restrict a) { L16(ASHR8) }
__attribute__((noinline)) void sextLshr8(s8* restrict d, const s8* restrict a) { L16(SEXT_LSHR8) }
__attribute__((noinline)) void
mul16(short* restrict e, const short* restrict x, const short* restrict y) {
	L4(MUL16, 0) L4(MUL16, 4)
}
__attribute__((noinline)) void ashr16(short* restrict e, const short* restrict x) {
	L4(ASHR16, 0) L4(ASHR16, 4)
}
__attribute__((noinline)) void trunc16(short* restrict e, const int* restrict w) {
	L4(TRUNC16, 0) L4(TRUNC16, 4)
}

int main(void) {
	s8 a[16], b[16], d[16];
	short x[8], y[8], e[8];
	int w[8];
	for(int base = 0; base < 256; base += 16) {
		for(int i = 0; i < 16; i++) {
			a[i] = (s8)(base + i);
			b[i] = (s8)(base * 7 + i * 29 + 3);
		}
		mul8(d, a, b);
		for(int i = 0; i < 16; i++)
			if(d[i] != (s8)(a[i] * b[i]))
				return 1;
		shl8(d, a);
		for(int i = 0; i < 16; i++)
			if(d[i] != (s8)(a[i] << 7))
				return 2;
		lshr8(d, a);
		for(int i = 0; i < 16; i++)
			if(d[i] != (s8)((u8)a[i] >> 3))
				return 3;
		ashr8(d, a);
		for(int i = 0; i < 16; i++)
			if(d[i] != (s8)(a[i] >> 1))
				return 4;
		sextLshr8(d, a);
		for(int i = 0; i < 16; i++)
			if(d[i] != (s8)((unsigned)a[i] >> 6))
				return 5;
		for(int i = 0; i < 8; i++) {
			x[i] = (short)(base * 257 + i * 4099);
			y[i] = (short)(base * 31 - i * 977);
			w[i] = base * 65793 + i * 40009 - 70000;
		}
		mul16(e, x, y);
		for(int i = 0; i < 8; i++)
			if(e[i] != (short)(x[i] * y[i]))
				return 6;
		ashr16(e, x);
		for(int i = 0; i < 8; i++)
			if(e[i] != (short)(x[i] >> 15))
				return 7;
		trunc16(e, w);
		for(int i = 0; i < 8; i++)
			if(e[i] != (short)w[i])
				return 8;
	}
	return 0;
}
