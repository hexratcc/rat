// expect: 0
// passes:

volatile unsigned short v16 = 0x1234u;
volatile unsigned int v32 = 0x12345678u;
volatile unsigned long long v64 = 0x0123456789abcdefull;

int main(void) {
	if(__builtin_bswap16(v16) != 0x3412u)
		return 1;
	if(__builtin_bswap32(v32) != 0x78563412u)
		return 2;
	if(__builtin_bswap64(v64) != 0xefcdab8967452301ull)
		return 3;
	// the 16-bit form must ignore whatever sits above the low halfword
	if(__builtin_bswap16((unsigned short)v32) != 0x7856u)
		return 4;
	return 0;
}
