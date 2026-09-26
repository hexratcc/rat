// expect: 0
// output:
//| -1294967296 -1 1 1
#include <stdio.h>

long double L = 3e9L, M = 255.0L, K = -2.0L;

int main(void) {
	unsigned u = (unsigned)L;
	unsigned char c = (unsigned char)M;
	short s = (short)K;
	long a = (int)u;
	long b = (signed char)c;
	printf("%ld %ld %d %d\n", a, b, u == 3000000000u, s == -2);
	return 0;
}
