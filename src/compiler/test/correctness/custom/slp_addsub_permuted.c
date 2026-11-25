// expect: 0
// add and sub lanes out of addsubps order pack as a shuffle of addsubps, so each lane keeps its own op.
// Both ops on every lane would compute inf - inf on the add lanes and raise invalid
int feclearexcept(int excepts);
int fetestexcept(int excepts);
#define FE_INVALID 1
#define FE_ALL_EXCEPT 0x3d

__attribute__((noinline)) void k(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[0] + b[0];
	d[1] = a[1] - b[1];
	d[2] = a[2] + b[2];
	d[3] = a[3] - b[3];
}

// quaternion product, three steps of mixed add and sub lanes
__attribute__((noinline)) void q(float* restrict r, const float* restrict p, const float* restrict s) {
	r[0] = p[0] * s[0] - p[1] * s[1] - p[2] * s[2] - p[3] * s[3];
	r[1] = p[0] * s[1] + p[1] * s[0] + p[2] * s[3] - p[3] * s[2];
	r[2] = p[0] * s[2] - p[1] * s[3] + p[2] * s[0] + p[3] * s[1];
	r[3] = p[0] * s[3] + p[1] * s[2] - p[2] * s[1] + p[3] * s[0];
}

int main(void) {
	float inf = 1.0f / 0.0f;
	float a[4] = {inf, 1, inf, 2}, b[4] = {inf, 5, inf, 7}, d[4];
	feclearexcept(FE_ALL_EXCEPT);
	k(d, a, b);
	int bad = fetestexcept(FE_INVALID) != 0;
	bad += d[0] != inf || d[1] != -4 || d[2] != inf || d[3] != -5;
	float p[4] = {1, 2, 3, 4}, s[4] = {5, 6, 7, 8}, r[4];
	q(r, p, s);
	return bad + (r[0] != -60) + (r[1] != 12) + (r[2] != 30) + (r[3] != 24);
}
