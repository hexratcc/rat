// expect: 0
// The repeated upper lanes of a 2-lane f32 vector survive a spill around a call. An 8-byte
// reload would zero them, then divps divides by -0 and mulps computes inf * 0
int feclearexcept(int excepts);
int fetestexcept(int excepts);
float fabsf(float x);
#define FE_ALL_EXCEPT 0x3d

__attribute__((noinline)) void ext(void) {}

__attribute__((noinline)) void neg(float* restrict d, const float* a, const float* restrict c) {
	float t0 = a[0], t1 = a[1];
	ext();
	d[0] = c[0] / -t0;
	d[1] = c[1] / -t1;
}

__attribute__((noinline)) void abs2(float* restrict d, const float* a, const float* restrict c) {
	float t0 = a[0], t1 = a[1];
	ext();
	d[0] = c[0] * fabsf(t0);
	d[1] = c[1] * fabsf(t1);
}

int main(void) {
	float inf = 1.0f / 0.0f;
	float a[2] = {1, 1}, c[2] = {1, 1}, e[2] = {inf, 1}, d[2], f[2];
	feclearexcept(FE_ALL_EXCEPT);
	neg(d, a, c);
	abs2(f, a, e);
	int raised = fetestexcept(FE_ALL_EXCEPT) != 0;
	return raised + (d[0] != -1) + (d[1] != -1) + (f[0] != inf) + (f[1] != 1);
}
