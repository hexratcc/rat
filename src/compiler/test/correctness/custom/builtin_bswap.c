// expect: 0

volatile unsigned short v16[4] = {0x1234u, 0x00ffu, 0xff00u, 0u};
volatile unsigned int v32[4] = {0x12345678u, 0x000000ffu, 0xff000000u, 0u};
volatile unsigned long long v64[3] = {
		0x0123456789abcdefull, 0x00000000000000ffull, 0xff00000000000000ull};

unsigned short w16[4] = {0x3412u, 0xff00u, 0x00ffu, 0u};
unsigned int w32[4] = {0x78563412u, 0xff000000u, 0x000000ffu, 0u};
unsigned long long w64[3] = {0xefcdab8967452301ull, 0xff00000000000000ull, 0x00000000000000ffull};

int main(void) {
	int i;
	for(i = 0; i < 4; ++i) {
		if(__builtin_bswap16(v16[i]) != w16[i])
			return 1;
		if(__builtin_bswap32(v32[i]) != w32[i])
			return 2;
	}
	for(i = 0; i < 3; ++i)
		if(__builtin_bswap64(v64[i]) != w64[i])
			return 3;
	// a round trip is the identity at every width
	if(__builtin_bswap16(__builtin_bswap16(v16[0])) != v16[0])
		return 4;
	if(__builtin_bswap32(__builtin_bswap32(v32[0])) != v32[0])
		return 5;
	if(__builtin_bswap64(__builtin_bswap64(v64[0])) != v64[0])
		return 6;
	// the result type is unsigned, so the top byte must not sign-extend
	if((unsigned long long)__builtin_bswap32(v32[1]) != 0xff000000ull)
		return 7;
	if(__builtin_bswap16(v16[2]) != 0x00ffu)
		return 8;
	return 0;
}
