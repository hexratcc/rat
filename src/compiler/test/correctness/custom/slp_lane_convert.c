// expect: 0
// lane-count-preserving converts pack: cvtdq2ps, cvttps2dq, cvtdq2pd, cvttpd2dq, cvtps2pd,
// cvtpd2ps, and pmovsx/pmovzx from one narrow load of adjacent i8/i16 lanes
int I[4], J[4];
float F[4], G[4];
double D[4], E[4];
short S[4];
unsigned char B[4];
signed char C[4];
unsigned short W[4];
long long L[2];
unsigned long long Q[2];

__attribute__((noinline)) void ints(float* restrict g, int* restrict j, const int* restrict i,
		const float* restrict f) {
	g[0] = (float)i[0] * 0.5f;
	g[1] = (float)i[1] * 0.5f;
	g[2] = (float)i[2] * 0.5f;
	g[3] = (float)i[3] * 0.5f;
	j[0] = (int)f[0] + 1;
	j[1] = (int)f[1] + 1;
	j[2] = (int)f[2] + 1;
	j[3] = (int)f[3] + 1;
}

__attribute__((noinline)) void doubles(double* restrict e, int* restrict j, float* restrict g,
		const int* restrict i, const double* restrict d, const float* restrict f) {
	e[0] = (double)i[0] + (double)f[0];
	e[1] = (double)i[1] + (double)f[1];
	j[0] = (int)d[0];
	j[1] = (int)d[1];
	g[0] = (float)d[2];
	g[1] = (float)d[3];
}

__attribute__((noinline)) void widen(int* restrict j, long long* restrict l, unsigned long long* restrict q,
		const short* restrict s, const unsigned char* restrict b, const signed char* restrict c,
		const unsigned short* restrict w, const int* restrict i) {
	j[0] = s[0] + b[0] + c[0] + w[0];
	j[1] = s[1] + b[1] + c[1] + w[1];
	j[2] = s[2] + b[2] + c[2] + w[2];
	j[3] = s[3] + b[3] + c[3] + w[3];
	l[0] = i[0];
	l[1] = i[1];
	q[0] = (unsigned)i[2];
	q[1] = (unsigned)i[3];
}

union U {
	double d[2];
	float f[4];
	int i[4];
};

// an f64 lane is wider than the f32 or i32 store of the run: the store to element 1 writes the
// upper half of d[0], which the lane-0 convert then reads
__attribute__((noinline)) void narrowf(union U* u) {
	u->f[1] = (float)u->d[1];
	u->f[0] = (float)u->d[0];
}

__attribute__((noinline)) void narrowi(union U* u) {
	u->i[1] = (int)u->d[1];
	u->i[0] = (int)u->d[0];
}

int main(void) {
	int iv[4] = {-7, 123456, -2147483647, 33};
	float fv[4] = {-2.75f, 1e9f, 0.5f, -1e-3f};
	for(int k = 0; k < 4; k++) {
		I[k] = iv[k];
		F[k] = fv[k];
		D[k] = (k - 1.5) * 1e5 + 0.123456789;
		S[k] = (short)(k * 20000 - 30000);
		B[k] = (unsigned char)(250 + k);
		C[k] = (signed char)(k * 60 - 128);
		W[k] = (unsigned short)(65535 - k * 1000);
	}
	ints(G, J, I, F);
	for(int k = 0; k < 4; k++)
		if(G[k] != (float)I[k] * 0.5f || J[k] != (int)F[k] + 1)
			return 1 + k;
	doubles(E, J, G, I, D, F);
	for(int k = 0; k < 2; k++)
		if(E[k] != (double)I[k] + (double)F[k] || J[k] != (int)D[k] || G[k] != (float)D[k + 2])
			return 10 + k;
	widen(J, L, Q, S, B, C, W, I);
	for(int k = 0; k < 4; k++)
		if(J[k] != S[k] + B[k] + C[k] + W[k])
			return 20 + k;
	if(L[0] != -7 || L[1] != 123456 || Q[0] != 2147483649ull || Q[1] != 33)
		return 30;
	union U u;
	u.d[0] = 1.0;
	u.d[1] = 2.0;
	narrowf(&u);
	if(u.f[0] != 2.0f || u.f[1] != 2.0f)
		return 40;
	u.d[0] = 3.0;
	u.d[1] = 1074790400.0; // 0x40100000, the upper half of 4.0
	narrowi(&u);
	if(u.i[0] != 4 || u.i[1] != 1074790400)
		return 41;
	return 0;
}
