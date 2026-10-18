// expect: 0
// selects on isomorphic compares pack as a compare mask and a blendv, a < b ? a : b and its mirror
// as pminsd/pmaxsd/pminud/pmaxud
int A[4], B[4], O[4], P[4];
unsigned U[4], V[4], X[4];
long long L[2], M[2], N[2];
float F[4], G[4];

__attribute__((noinline)) void minmax(int* restrict o, int* restrict p, unsigned* restrict x,
		const int* restrict a, const int* restrict b, const unsigned* restrict u,
		const unsigned* restrict v) {
	o[0] = a[0] < b[0] ? a[0] : b[0];
	o[1] = a[1] < b[1] ? a[1] : b[1];
	o[2] = a[2] < b[2] ? a[2] : b[2];
	o[3] = a[3] < b[3] ? a[3] : b[3];
	p[0] = a[0] <= b[0] ? b[0] : a[0];
	p[1] = a[1] <= b[1] ? b[1] : a[1];
	p[2] = a[2] <= b[2] ? b[2] : a[2];
	p[3] = a[3] <= b[3] ? b[3] : a[3];
	x[0] = u[0] > v[0] ? v[0] : u[0];
	x[1] = u[1] > v[1] ? v[1] : u[1];
	x[2] = u[2] > v[2] ? v[2] : u[2];
	x[3] = u[3] > v[3] ? v[3] : u[3];
}

__attribute__((noinline)) void blends(int* restrict o, unsigned* restrict x, const int* restrict a,
		const int* restrict b, const unsigned* restrict u, const unsigned* restrict v, int k) {
	o[0] = a[0] <= b[0] ? k : a[0] ^ b[0];
	o[1] = a[1] <= b[1] ? k : a[1] ^ b[1];
	o[2] = a[2] <= b[2] ? k : a[2] ^ b[2];
	o[3] = a[3] <= b[3] ? k : a[3] ^ b[3];
	x[0] = u[0] < v[0] ? 7u : u[0] == v[0] ? 0u : 9u;
	x[1] = u[1] < v[1] ? 7u : u[1] == v[1] ? 0u : 9u;
	x[2] = u[2] < v[2] ? 7u : u[2] == v[2] ? 0u : 9u;
	x[3] = u[3] < v[3] ? 7u : u[3] == v[3] ? 0u : 9u;
}

__attribute__((noinline)) void wide(long long* restrict n, int* restrict p, const long long* restrict l,
		const long long* restrict m, const float* restrict f, const float* restrict g) {
	n[0] = l[0] != m[0] ? l[0] : 42;
	n[1] = l[1] != m[1] ? l[1] : 42;
	p[0] = f[0] >= g[0] ? 1 : -1;
	p[1] = f[1] >= g[1] ? 1 : -1;
	p[2] = f[2] >= g[2] ? 1 : -1;
	p[3] = f[3] >= g[3] ? 1 : -1;
}

int main(void) {
	int av[4] = {-5, 7, 2147483647, 0};
	int bv[4] = {3, 7, -2147483647, -1};
	unsigned uv[4] = {0xffffffffu, 3u, 0x80000000u, 9u};
	unsigned vv[4] = {1u, 3u, 0x7fffffffu, 10u};
	float fv[4] = {1.0f, -0.0f, 0.0f / 0.0f, 2.0f};
	float gv[4] = {2.0f, 0.0f, 1.0f, 2.0f};
	for(int k = 0; k < 4; k++) {
		A[k] = av[k];
		B[k] = bv[k];
		U[k] = uv[k];
		V[k] = vv[k];
		F[k] = fv[k];
		G[k] = gv[k];
	}
	minmax(O, P, X, A, B, U, V);
	for(int k = 0; k < 4; k++) {
		int lo = A[k] < B[k] ? A[k] : B[k];
		int hi = A[k] <= B[k] ? B[k] : A[k];
		unsigned ulo = U[k] > V[k] ? V[k] : U[k];
		if(O[k] != lo || P[k] != hi || X[k] != ulo)
			return 1 + k;
	}
	blends(O, X, A, B, U, V, 11);
	for(int k = 0; k < 4; k++) {
		int want = A[k] <= B[k] ? 11 : A[k] ^ B[k];
		unsigned uw = U[k] < V[k] ? 7u : U[k] == V[k] ? 0u : 9u;
		if(O[k] != want || X[k] != uw)
			return 10 + k;
	}
	L[0] = 5;
	L[1] = -9000000000LL;
	M[0] = 5;
	M[1] = 9000000000LL;
	wide(N, P, L, M, F, G);
	if(N[0] != 42 || N[1] != -9000000000LL)
		return 20;
	for(int k = 0; k < 4; k++)
		if(P[k] != (F[k] >= G[k] ? 1 : -1))
			return 30 + k;
	return 0;
}
