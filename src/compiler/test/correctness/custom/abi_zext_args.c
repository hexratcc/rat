// expect: 15
// host-side:
// unsigned narrow int arguments to a host callee, clang callees read them zero extended to 32 bits
// the host side reads all 32 bits, a gcc callee would extend them again
#ifdef RAT_HOST_SIDE
unsigned g_uchar(unsigned x) { return x; }
unsigned g_ushort(unsigned x) { return x; }
unsigned g_uchar7(long a, long b, long c, long d, long e, long f, unsigned x) { return x; }
#else
unsigned g_uchar(unsigned char c);
unsigned g_ushort(unsigned short s);
unsigned g_uchar7(long a, long b, long c, long d, long e, long f, unsigned char c);

int main(void) {
	volatile unsigned char c = 255;
	volatile unsigned short s = 65535;
	volatile int n = 5;
	int bits = 0;
	if(g_uchar(c) == 255)
		bits |= 1;
	if(g_ushort(s) == 65535)
		bits |= 2;
	if(g_uchar7(0, 0, 0, 0, 0, 0, c) == 255)
		bits |= 4;
	if(g_uchar((unsigned char)(n * 307)) == 255)
		bits |= 8;
	return bits;
}
#endif
