// expect: 0
// restrict parameters pack statically: the loads through r, g and b hoist past the stores
// through y; a and c may alias each other because nothing modifies the object they share
float Y[8], R[8], G[8], B[8];
unsigned D[8], A[8];

__attribute__((noinline)) void gray(float* restrict y, const float* restrict r,
		const float* restrict g, const float* restrict b) {
	y[0] = 0.25f * r[0] + 0.5f * g[0] + 0.125f * b[0];
	y[1] = 0.25f * r[1] + 0.5f * g[1] + 0.125f * b[1];
	y[2] = 0.25f * r[2] + 0.5f * g[2] + 0.125f * b[2];
	y[3] = 0.25f * r[3] + 0.5f * g[3] + 0.125f * b[3];
}

__attribute__((noinline)) void mix(unsigned* restrict d, const unsigned* restrict a,
		const unsigned* restrict c) {
	d[0] = a[0] * c[1] + 1;
	d[1] = a[1] * c[2] + 2;
	d[2] = a[2] * c[3] + 3;
	d[3] = a[3] * c[4] + 4;
}

int main(void) {
	for(int i = 0; i < 8; i++) {
		R[i] = (float)(4 * i);
		G[i] = (float)(2 * i + 1);
		B[i] = (float)(8 * i);
		A[i] = (unsigned)(i + 3);
	}
	gray(Y, R, G, B);
	for(int i = 0; i < 4; i++)
		if(Y[i] != 0.25f * R[i] + 0.5f * G[i] + 0.125f * B[i])
			return 1 + i;
	mix(D, A, A);
	for(int i = 0; i < 4; i++)
		if(D[i] != A[i] * A[i + 1] + (unsigned)(i + 1))
			return 10 + i;
	return 0;
}
