// expect: 0
// Memory to memory fabs and fneg work on the int bits, so NaN payloads and -0 survive
float fabsf(float x);
double fabs(double x);
typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((noinline)) void k(float* restrict d, const float* restrict a, double* restrict e, const double* restrict b) {
	d[0] = fabsf(a[0]);
	d[1] = -a[1];
	e[0] = fabs(b[0]);
	e[1] = -b[1];
}

int main(void) {
	union { float f[2]; u32 u[2]; } a = {.u = {0xffc12345u, 0x80000000u}}, d;
	union { double f[2]; u64 u[2]; } b = {.u = {0xfff8000000abcdefull, 0x0000000000000001ull}}, e;
	k(d.f, a.f, e.f, b.f);
	int bad = d.u[0] != 0x7fc12345u || d.u[1] != 0u;
	return bad + (e.u[0] != 0x7ff8000000abcdefull) + (e.u[1] != 0x8000000000000001ull);
}
