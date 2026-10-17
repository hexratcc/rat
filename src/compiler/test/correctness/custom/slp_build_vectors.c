// expect: 0
// a vector of variable and constant lanes is built in registers: the constant lanes from the pool
// with an insert per variable lane, or lane 0 into zeros when every constant is zero; float lanes
// gather with insertps, movsd and movlhps instead of through the stack
typedef struct {
	int kind, a, b, c;
} Op;

__attribute__((noinline)) void mk(Op* restrict o, int x) {
	o->kind = x;
	o->a = 0;
	o->b = 0;
	o->c = 0;
}

__attribute__((noinline)) void mk2(Op* restrict o, int x, int y) {
	o->kind = 7;
	o->a = x;
	o->b = 0;
	o->c = y;
}

__attribute__((noinline)) void ptrs(void** restrict p, void* x) {
	p[0] = 0;
	p[1] = x;
}

__attribute__((noinline)) void gat(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] * b[0];
	d[1] = a[4] * b[1];
	d[2] = a[8] * b[2];
	d[3] = a[12] * b[3];
}

__attribute__((noinline)) void fz(float* restrict d, float x, float y) {
	d[0] = x;
	d[1] = 0.0f;
	d[2] = y;
	d[3] = -0.0f;
}

__attribute__((noinline)) void dz(double* restrict d, double x) {
	d[0] = x;
	d[1] = 0.0;
}

__attribute__((noinline)) void zd(double* restrict d, double x) {
	d[0] = 0.0;
	d[1] = x;
}

__attribute__((noinline)) void dc(double* restrict d, const double* restrict a, double x) {
	d[0] = a[0] + x;
	d[1] = a[1] + 2.5;
}

__attribute__((noinline)) void lc(long long* restrict d, const long long* restrict a, long long x) {
	d[0] = a[0] + 5;
	d[1] = a[1] + x;
}

__attribute__((noinline)) void fmix(float* restrict d, const float* restrict a, const float* restrict b,
																		float x, float y) {
	d[0] = a[0] * x + b[0];
	d[1] = a[1] * 0.0f + b[1];
	d[2] = a[2] * y + b[2];
	d[3] = a[3] * -0.0f + b[3];
}

__attribute__((noinline)) void fzero(float* restrict d, const float* restrict a, const float* restrict b,
																		 float x) {
	d[0] = a[0] + b[0] * x;
	d[1] = a[1] + b[1] * 0.0f;
	d[2] = a[2] + b[2] * 0.0f;
	d[3] = a[3] + b[3] * 0.0f;
}

__attribute__((noinline)) void dmix(double* restrict d, const double* restrict a, const double* restrict b,
																		double x) {
	d[0] = a[0] + b[0] * x;
	d[1] = a[1] + b[1] * 0.0;
	d[2] = a[2] + b[2] * 0.0;
	d[3] = a[3] + b[3] * x;
	d[4] = a[4] + b[4] * x;
	d[5] = a[5] + b[5] * 2.5;
}

__attribute__((noinline)) void lmix(long long* restrict d, const long long* restrict a,
																		const long long* restrict c, long long x) {
	d[0] = (a[0] + c[0]) ^ 5;
	d[1] = (a[1] + c[1]) ^ x;
}

__attribute__((noinline)) void imix(int* restrict d, const int* restrict a, int x, int y) {
	d[0] = a[0] + x;
	d[1] = a[1] + 0;
	d[2] = a[2] + y;
	d[3] = a[3] - 0;
}

int bits(float f) {
	union { float f; int i; } u = {f};
	return u.i;
}

int main(void) {
	Op o = {1, 2, 3, 4};
	mk(&o, -9);
	if(o.kind != -9 || o.a || o.b || o.c)
		return 1;
	mk2(&o, 5, -6);
	if(o.kind != 7 || o.a != 5 || o.b || o.c != -6)
		return 2;
	void* p[2] = {&o, &o};
	ptrs(p, p);
	if(p[0] || p[1] != (void*)p)
		return 3;
	float a[16], b[4], d[4];
	for(int i = 0; i < 16; i++)
		a[i] = (float)i * 0.5f - 3.0f;
	for(int i = 0; i < 4; i++)
		b[i] = (float)(i + 2);
	gat(d, a, b);
	for(int i = 0; i < 4; i++)
		if(d[i] != a[4 * i] * b[i])
			return 4;
	fz(d, 1.25f, -7.5f);
	if(d[0] != 1.25f || bits(d[1]) != 0 || d[2] != -7.5f || bits(d[3]) != (int)0x80000000)
		return 5;
	double e[2] = {1.0, 1.0};
	dz(e, -3.25);
	if(e[0] != -3.25 || e[1] != 0.0)
		return 6;
	zd(e, 6.5);
	if(e[0] != 0.0 || e[1] != 6.5)
		return 7;
	double da[2] = {1.5, -4.0};
	dc(e, da, 0.25);
	if(e[0] != 1.75 || e[1] != -1.5)
		return 8;
	long long la[2] = {1LL << 40, -3}, ld[2];
	lc(ld, la, 1LL << 33);
	if(ld[0] != (1LL << 40) + 5 || ld[1] != (1LL << 33) - 3)
		return 9;
	float fa[4] = {1.0f, -2.0f, 3.0f, 4.0f}, fb[4] = {0.5f, 0.25f, -8.0f, 16.0f};
	fmix(d, fa, fb, 3.0f, -1.0f);
	if(d[0] != 3.5f || d[1] != 0.25f || d[2] != -11.0f || d[3] != 16.0f)
		return 10;
	fzero(d, fa, fb, 4.0f);
	if(d[0] != 3.0f || d[1] != -2.0f || d[2] != 3.0f || d[3] != 4.0f)
		return 11;
	double xa[6] = {1, 2, 3, 4, 5, 6}, xb[6] = {0.5, 1, 1.5, 2, 2.5, 3}, xd[6];
	dmix(xd, xa, xb, 2.0);
	if(xd[0] != 2.0 || xd[1] != 2.0 || xd[2] != 3.0 || xd[3] != 8.0 || xd[4] != 10.0 || xd[5] != 13.5)
		return 12;
	long long lb[2] = {7, 9};
	lmix(ld, la, lb, 1LL << 35);
	if(ld[0] != (((1LL << 40) + 7) ^ 5) || ld[1] != (6 ^ (1LL << 35)))
		return 13;
	int ia[4] = {10, 20, 30, 40}, id[4];
	imix(id, ia, -5, 8);
	if(id[0] != 5 || id[1] != 20 || id[2] != 38 || id[3] != 40)
		return 14;
	return 0;
}
