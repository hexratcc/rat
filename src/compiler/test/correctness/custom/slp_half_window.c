// expect: 0
// two adjacent 32-bit stores pack as one 8-byte window: movsd loads and store, the op in the low
// half of the register; a third store and a store past a hole stay scalar
int D[8], A[8], B[8];
float F[4], G[4];

__attribute__((noinline)) void three(int* restrict d, const int* restrict a, const int* restrict b) {
	d[0] = a[0] - b[0];
	d[1] = a[1] - b[1];
	d[2] = a[2] - b[2];
	d[4] = a[4] - b[4];
}

__attribute__((noinline)) void scale(float* restrict f, const float* restrict g) {
	f[0] = g[0] * 0.5f + 1.0f;
	f[1] = g[1] * 0.5f + 2.0f;
}

__attribute__((noinline)) void inplace(int* p) {
	p[1] = p[0] * 3;
	p[2] = p[1] * 3;
}

int main(void) {
	for(int i = 0; i < 8; i++) {
		A[i] = 10 * i + 7;
		B[i] = i * i;
		D[i] = -1;
	}
	three(D, A, B);
	for(int i = 0; i < 5; i++)
		if(D[i] != (i == 3 ? -1 : A[i] - B[i]))
			return 1 + i;
	G[0] = 3.0f;
	G[1] = -5.0f;
	scale(F, G);
	if(F[0] != 2.5f || F[1] != -0.5f)
		return 10;
	inplace(A);
	if(A[1] != 21 || A[2] != 63)
		return 11;
	return 0;
}
