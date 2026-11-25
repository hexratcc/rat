// expect: 0
// sub/add lanes lower to addsubps even when gvn swaps the operands of the add.
// subps plus addps would compute inf - inf on the add lanes and raise invalid
// rat has no fenv.h, these come from libm
int feclearexcept(int excepts);
int fetestexcept(int excepts);
#define FE_INVALID 1
#define FE_ALL_EXCEPT 0x3d

__attribute__((noinline)) void k(float* restrict d, float* restrict e, const float* restrict a, const float* restrict b) {
	e[0] = b[0] + a[0];
	e[1] = b[1] + a[1];
	e[2] = b[2] + a[2];
	e[3] = b[3] + a[3];
	d[0] = a[0] - b[0];
	d[1] = a[1] + b[1];
	d[2] = a[2] - b[2];
	d[3] = a[3] + b[3];
}

int main(void) {
	float inf = 1.0f / 0.0f;
	float a[4] = {1, inf, 2, 4}, b[4] = {1, inf, 2, 4}, d[4], e[4];
	feclearexcept(FE_ALL_EXCEPT);
	k(d, e, a, b);
	int invalid = fetestexcept(FE_INVALID) != 0;
	return invalid + (d[1] != inf) + (e[1] != inf);
}
