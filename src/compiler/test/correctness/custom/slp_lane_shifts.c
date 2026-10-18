// expect: 0
// shifts by one amount in every lane pack as pslld/psrld/psrad/psllq/psrlq, by an immediate or by a
// count register; each result is checked against a scalar loop
int I[4], O[4];
unsigned U[4], V[4];
long long L[2], M[2];
unsigned long long Q[2], R[2];

__attribute__((noinline)) void imm32(int* restrict o, unsigned* restrict v, const int* restrict i,
		const unsigned* restrict u) {
	o[0] = i[0] >> 5;
	o[1] = i[1] >> 5;
	o[2] = i[2] >> 5;
	o[3] = i[3] >> 5;
	v[0] = (u[0] << 7) ^ (u[0] >> 3);
	v[1] = (u[1] << 7) ^ (u[1] >> 3);
	v[2] = (u[2] << 7) ^ (u[2] >> 3);
	v[3] = (u[3] << 7) ^ (u[3] >> 3);
}

__attribute__((noinline)) void var32(int* restrict o, const int* restrict i, int s) {
	o[0] = (int)((unsigned)i[0] << s) ^ (i[0] >> s);
	o[1] = (int)((unsigned)i[1] << s) ^ (i[1] >> s);
	o[2] = (int)((unsigned)i[2] << s) ^ (i[2] >> s);
	o[3] = (int)((unsigned)i[3] << s) ^ (i[3] >> s);
}

__attribute__((noinline)) void imm64(long long* restrict m, unsigned long long* restrict r,
		const long long* restrict l, const unsigned long long* restrict q, int s) {
	m[0] = (long long)((unsigned long long)l[0] << 9);
	m[1] = (long long)((unsigned long long)l[1] << 9);
	r[0] = q[0] >> s;
	r[1] = q[1] >> s;
}

int main(void) {
	int iv[4] = {-123456789, 987654, -1, 0x7fffffff};
	unsigned uv[4] = {0xdeadbeefu, 1u, 0x80000000u, 12345u};
	for(int k = 0; k < 4; k++) {
		I[k] = iv[k];
		U[k] = uv[k];
	}
	L[0] = -77777777777LL;
	L[1] = 5LL;
	Q[0] = 0xfedcba9876543210ull;
	Q[1] = 3ull;
	imm32(O, V, I, U);
	for(int k = 0; k < 4; k++)
		if(O[k] != I[k] >> 5 || V[k] != ((U[k] << 7) ^ (U[k] >> 3)))
			return 1 + k;
	for(int s = 0; s < 31; s += 6) {
		var32(O, I, s);
		for(int k = 0; k < 4; k++)
			if(O[k] != ((int)((unsigned)I[k] << s) ^ (I[k] >> s)))
				return 10 + k;
	}
	for(int s = 0; s < 64; s += 13) {
		imm64(M, R, L, Q, s);
		for(int k = 0; k < 2; k++)
			if(M[k] != (long long)((unsigned long long)L[k] << 9) || R[k] != Q[k] >> s)
				return 20 + k;
	}
	return 0;
}
