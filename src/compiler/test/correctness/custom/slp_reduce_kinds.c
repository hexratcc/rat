// expect: 0
// and, or, xor and mul chains reduce like add chains (exact on integers in any order), a four-term
// dot product as one vector with a horizontal finish, and the remainder terms after the vectors
__attribute__((noinline)) int rand8(const int* restrict a) {
	return a[0] & a[1] & a[2] & a[3] & a[4] & a[5] & a[6] & a[7];
}

__attribute__((noinline)) unsigned ror9(const unsigned* restrict a) {
	return a[0] | a[1] | a[2] | a[3] | a[4] | a[5] | a[6] | a[7] | a[8];
}

__attribute__((noinline)) long long rxor8(const long long* restrict a) {
	return a[0] ^ a[1] ^ a[2] ^ a[3] ^ a[4] ^ a[5] ^ a[6] ^ a[7];
}

__attribute__((noinline)) int rmul8(const int* restrict a) {
	return a[0] * a[1] * a[2] * a[3] * a[4] * a[5] * a[6] * a[7];
}

__attribute__((noinline)) void dot4(int* restrict d, const int* restrict a, const int* restrict b) {
	d[0] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

__attribute__((noinline)) int mulpair(const int* restrict a, const int* restrict b) {
	return (a[0] + b[0]) * (a[1] + b[1]) * (a[2] + b[2]) * (a[3] + b[3]);
}

int main(void) {
	int a[9], b[9];
	unsigned u[9];
	long long l[8];
	for(int i = 0; i < 9; i++) {
		a[i] = (i * 37 + 11) | 0x101;
		b[i] = 5 - 3 * i;
		u[i] = 1u << (i * 3 % 31);
	}
	for(int i = 0; i < 8; i++)
		l[i] = (long long)(i + 1) * 0x123456789ll;
	int r = a[0];
	for(int i = 1; i < 8; i++)
		r &= a[i];
	if(rand8(a) != r)
		return 1;
	unsigned o = 0;
	for(int i = 0; i < 9; i++)
		o |= u[i];
	if(ror9(u) != o)
		return 2;
	long long x = 0;
	for(int i = 0; i < 8; i++)
		x ^= l[i];
	if(rxor8(l) != x)
		return 3;
	unsigned m = 1;
	for(int i = 0; i < 8; i++)
		m *= (unsigned)a[i];
	if(rmul8(a) != (int)m)
		return 4;
	int d[1];
	dot4(d, a, b);
	if(d[0] != a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])
		return 5;
	if(mulpair(a, b) != (a[0] + b[0]) * (a[1] + b[1]) * (a[2] + b[2]) * (a[3] + b[3]))
		return 6;
	return 0;
}
