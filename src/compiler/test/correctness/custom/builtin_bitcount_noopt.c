// expect: 0
// passes:

volatile unsigned v32[4] = {1u, 0x80000000u, 0xffffffffu, 42u};
volatile unsigned long long v64[4] = {
		1ull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x100000000ull};

int clz32[4] = {31, 0, 0, 26};
int ctz32[4] = {0, 31, 0, 1};
int pop32[4] = {1, 1, 32, 3};
int ffs32[4] = {1, 32, 1, 2};

int clz64[4] = {63, 0, 0, 31};
int ctz64[4] = {0, 63, 0, 32};
int pop64[4] = {1, 1, 64, 1};
int ffs64[4] = {1, 64, 1, 33};

int main(void) {
	int i;
	for(i = 0; i < 4; ++i) {
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
	for(i = 0; i < 4; ++i) {
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
	if(__builtin_ffs((int)v32[0] - 1) != 0)
		return 9;
	if(__builtin_ffsll((long long)v64[0] - 1) != 0)
		return 10;
	return 0;
}
