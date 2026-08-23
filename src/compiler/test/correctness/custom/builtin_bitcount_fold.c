// expect: 42

int main(void) {
	int r = 0;
	r += __builtin_clz(1u);														// 31
	r += __builtin_ctz(0x80000000u);									// 31
	r += __builtin_popcount(0xffffffffu);							// 32
	r += __builtin_clzll(1ull);												// 63
	r += __builtin_ctzll(0x100000000ull);							// 32
	r += __builtin_popcountll(0x5555555555555555ull); // 32
	r += __builtin_ffs(0);														// 0
	r += __builtin_ffsll(0x40ll);											// 7
	return r - 186;																		// 228 - 186 == 42
}
