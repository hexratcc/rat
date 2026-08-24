// expect: 42

int main(void) {
	int r = 0;
	r += __builtin_bswap16(0x2a00u);							// 42
	r += (int)(__builtin_bswap32(0x2au) >> 24);		// 42
	r += (int)(__builtin_bswap64(0x2aull) >> 56); // 42
	return r - 84;																// 126 - 84 == 42
}
