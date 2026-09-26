// expect: 0
// output:
//| 2 1
//| 6
#include <stdint.h>
#include <stdio.h>

long double A = 1.0L, B = 2.0L;
int N = 3;

int main(void) {
	long double x = A, y = B;
	for(int i = 0; i < N; i++) {
		long double t = x;
		x = y;
		y = t;
	}
	printf("%Lg %Lg\n", x, y);

	long double p = 0, q = 0;
	uint64_t r = 0;
	for(int i = 0; i < N; i++) {
		p;
		r ^= (uint64_t)q;
		q = p;
		p = 6;
	}
	printf("%llu\n", (unsigned long long)r);
	return 0;
}
