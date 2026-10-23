// expect: 0
// lanes of two opcodes pack as both vector ops and a blend (addsubps for a difference in the even
// lanes and a sum in the odd ones). The other functions mix x + c with x - c lanes, or have a lane
// that is just x, and must give exact results, -0.0 lanes included
int signbitf(float x) {
	union { float f; unsigned u; } v = {x};
	return v.u >> 31;
}

__attribute__((noinline)) void addsub(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] + b[0];
	d[1] = a[1] - b[1];
	d[2] = a[2] + b[2];
	d[3] = a[3] - b[3];
}

__attribute__((noinline)) void subadd(double* restrict d, const double* restrict a, const double* restrict b) {
	d[0] = a[0] - b[0];
	d[1] = a[1] + b[1];
}

__attribute__((noinline)) void addmul(int* restrict d, const int* restrict a, const int* restrict b) {
	d[0] = a[0] + b[0];
	d[1] = a[1] * b[1];
	d[2] = a[2] + b[2];
	d[3] = a[3] * b[3];
}

__attribute__((noinline)) void subadd64(long long* restrict d, const long long* restrict a,
																				const long long* restrict b) {
	d[0] = a[0] - b[0];
	d[1] = a[1] + b[1];
}

typedef struct {
	unsigned len, large, v1, v2, v3, v4;
} State;

__attribute__((noinline)) void reset(State* restrict s, unsigned seed) {
	s->v1 = seed + 2654435761U + 2246822519U;
	s->v2 = seed + 2246822519U;
	s->v3 = seed + 0;
	s->v4 = seed - 2654435761U;
}

__attribute__((noinline)) void copyf(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] + b[0];
	d[1] = a[1];
	d[2] = a[2] + b[2];
	d[3] = a[3] + b[3];
}

__attribute__((noinline)) void mulcopy(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] * b[0];
	d[1] = a[1] * b[1];
	d[2] = a[2];
	d[3] = a[3] * b[3];
}

int main(void) {
	float a[4] = {1.5f, -2.25f, 3.0f, 0.5f}, b[4] = {0.25f, 4.0f, -1.0f, 8.0f}, d[4];
	addsub(d, a, b);
	if(d[0] != a[0] + b[0] || d[1] != a[1] - b[1] || d[2] != a[2] + b[2] || d[3] != a[3] - b[3])
		return 1;
	double da[2] = {1e300, -3.5}, db[2] = {-1e300, 0.125}, dd[2];
	subadd(dd, da, db);
	if(dd[0] != da[0] - db[0] || dd[1] != da[1] + db[1])
		return 2;
	int ia[4] = {7, -3, 100000, 46341}, ib[4] = {-9, 11, 5, 46341}, id[4];
	addmul(id, ia, ib);
	if(id[0] != ia[0] + ib[0] || id[1] != ia[1] * ib[1] || id[2] != ia[2] + ib[2] ||
		 id[3] != (int)((unsigned)ia[3] * (unsigned)ib[3]))
		return 3;
	long long la[2] = {1LL << 40, -5}, lb[2] = {3, 1LL << 50}, ld[2];
	subadd64(ld, la, lb);
	if(ld[0] != la[0] - lb[0] || ld[1] != la[1] + lb[1])
		return 4;
	State s = {1, 2, 0, 0, 0, 0};
	reset(&s, 0x9e3779b1u);
	if(s.v1 != 0x9e3779b1u + 2654435761U + 2246822519U || s.v2 != 0x9e3779b1u + 2246822519U ||
		 s.v3 != 0x9e3779b1u || s.v4 != 0x9e3779b1u - 2654435761U || s.len != 1 || s.large != 2)
		return 5;
	float z[4] = {-0.0f, -0.0f, 2.0f, 1.0f}, nz[4] = {0.0f, 7.0f, -0.0f, -0.0f};
	copyf(d, z, nz);
	if(!signbitf(d[1]) || signbitf(d[0]) || d[2] != 2.0f || d[3] != 1.0f)
		return 6;
	float q[4] = {2.0f, 3.0f, -0.0f, 5.0f};
	mulcopy(d, q, a);
	if(d[0] != q[0] * a[0] || d[1] != q[1] * a[1] || !signbitf(d[2]) || d[2] != 0.0f || d[3] != q[3] * a[3])
		return 7;
	return 0;
}
