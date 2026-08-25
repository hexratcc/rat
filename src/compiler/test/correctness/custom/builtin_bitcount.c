// expect: 0

volatile unsigned v32[5] = {1u, 0x80000000u, 0xffffffffu, 0x00ff0000u, 42u};
volatile unsigned long long v64[5] = {
		1ull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x100000000ull, 0x5555555555555555ull};

int clz32[5] = {31, 0, 0, 8, 26};
int ctz32[5] = {0, 31, 0, 16, 1};
int pop32[5] = {1, 1, 32, 8, 3};
int ffs32[5] = {1, 32, 1, 17, 2};

int clz64[5] = {63, 0, 0, 31, 1};
int ctz64[5] = {0, 63, 0, 32, 0};
int pop64[5] = {1, 1, 64, 1, 32};
int ffs64[5] = {1, 64, 1, 33, 1};

int main(void) {
	int i;
	int longBits = (int)(sizeof(unsigned long) * 8);
	volatile unsigned long l = 0x1234u;

	for(i = 0; i < 5; ++i) {
		unsigned x = v32[i];
		if(__builtin_clz(x) != clz32[i])
			return 1;
		if(__builtin_ctz(x) != ctz32[i])
			return 2;
		if(__builtin_popcount(x) != pop32[i])
			return 3;
		if(__builtin_ffs((int)x) != ffs32[i])
			return 4;
	}
	for(i = 0; i < 5; ++i) {
		unsigned long long x = v64[i];
		if(__builtin_clzll(x) != clz64[i])
			return 5;
		if(__builtin_ctzll(x) != ctz64[i])
			return 6;
		if(__builtin_popcountll(x) != pop64[i])
			return 7;
		if(__builtin_ffsll((long long)x) != ffs64[i])
			return 8;
	}
	// the 'l' forms follow the target's long width
	if(__builtin_clzl(l) != longBits - 13)
		return 9;
	if(__builtin_ctzl(l) != 2)
		return 10;
	if(__builtin_popcountl(l) != 5)
		return 11;
	if(__builtin_ffsl((long)l) != 3)
		return 12;

	// ffs is defined at zero for every width
	if(__builtin_ffs((int)v32[0] - 1) != 0)
		return 13;
	if(__builtin_ffsll((long long)v64[0] - 1) != 0)
		return 14;
	return 0;
}
