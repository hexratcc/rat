// expect: 0
// An int select by a 2-lane f32 order compare stays scalar.
// cmpltps signals on a quiet NaN, our ucomiss does not
int feclearexcept(int excepts);
int fetestexcept(int excepts);
#define FE_ALL_EXCEPT 0x3d

__attribute__((noinline)) void k(int* restrict r, const float* restrict a, const float* restrict b) {
	r[0] = a[0] < b[0] ? 3 : 9;
	r[1] = a[1] < b[1] ? 4 : 11;
}

int main(void) {
	float a[2] = {0.0f / 0.0f, 1}, b[2] = {1, 2};
	int r[2];
	feclearexcept(FE_ALL_EXCEPT);
	k(r, a, b);
	return (fetestexcept(FE_ALL_EXCEPT) != 0) + (r[0] != 9) + (r[1] != 4);
}
