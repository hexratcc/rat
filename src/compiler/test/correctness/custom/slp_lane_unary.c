// expect: 0
// negation and complement pack as a subtract from a zero vector and an xor with all ones, fneg as a
// sign-bit xor that also flips zeros
int I[4], O[4];
long long L[2], M[2];
float F[4], G[4];
double D[2], E[2];

__attribute__((noinline)) void ints(int* restrict o, long long* restrict m, const int* restrict i,
		const long long* restrict l) {
	o[0] = -i[0] + ~i[1];
	o[1] = -i[1] + ~i[2];
	o[2] = -i[2] + ~i[3];
	o[3] = -i[3] + ~i[0];
	m[0] = ~l[0];
	m[1] = ~l[1];
}

__attribute__((noinline)) void floats(float* restrict g, double* restrict e, const float* restrict f,
		const double* restrict d) {
	g[0] = -f[0];
	g[1] = -f[1];
	g[2] = -f[2];
	g[3] = -f[3];
	e[0] = -(d[0] * 2.0);
	e[1] = -(d[1] * 2.0);
}

int main(void) {
	int iv[4] = {5, -2147483647, 0, 99};
	for(int k = 0; k < 4; k++)
		I[k] = iv[k];
	L[0] = -9000000000LL;
	L[1] = 0;
	ints(O, M, I, L);
	for(int k = 0; k < 4; k++)
		if(O[k] != (int)(0u - (unsigned)I[k]) + ~I[(k + 1) % 4])
			return 1 + k;
	if(M[0] != ~L[0] || M[1] != -1)
		return 5;
	float fv[4] = {1.5f, -0.0f, 0.0f, -3e38f};
	for(int k = 0; k < 4; k++)
		F[k] = fv[k];
	D[0] = 0.0;
	D[1] = -2.5;
	floats(G, E, F, D);
	for(int k = 0; k < 4; k++)
		if(G[k] != -F[k] || 1.0f / G[k] == 1.0f / F[k])
			return 10 + k; // also -0 against +0
	if(1.0 / E[0] > 0 || E[1] != 5.0)
		return 20;
	return 0;
}
