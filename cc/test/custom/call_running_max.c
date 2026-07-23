// expect: 0
#include <math.h>
#define N 220
static double a[N * N];
int main(void) {
	int i, k, bad = 0;
	for (i = 0; i < N * N; i++)
		a[i] = (double)(i % 21) - 10.0;
	for (k = 0; k < N; k++) {
		double bv = fabs(a[k * N + k]);
		for (i = k + 1; i < N; i++) {
			double v = fabs(a[i * N + k]);
			if (v > bv)
				bv = v;
		}
		if (bv == 0.0)
			bad++;
	}
	return bad;
}
