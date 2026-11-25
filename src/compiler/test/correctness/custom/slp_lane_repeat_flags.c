// expect: 0
// A 2-lane f32 divide packs into divps. Lanes 2 and 3 repeat a / b.
// Zero upper lanes would compute 0 / 0 and raise invalid
// rat has no fenv.h, these come from libm
int feclearexcept(int excepts);
int fetestexcept(int excepts);
#define FE_ALL_EXCEPT 0x3d

__attribute__((noinline)) void k(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] / b[0];
	d[1] = a[1] / b[1];
}

__attribute__((noinline)) float pair(const float* restrict a, const float* restrict b) {
	return (a[0] * b[0] + 1.0f) * (a[1] * b[1] + 1.0f);
}

int main(void) {
	float a[2] = {1, 3}, b[2] = {2, 4}, d[2];
	feclearexcept(FE_ALL_EXCEPT);
	k(d, a, b);
	float p = pair(a, b);
	int raised = fetestexcept(FE_ALL_EXCEPT) != 0;
	return raised + (d[0] != 0.5f) + (d[1] != 0.75f) + (p != 39.0f);
}
