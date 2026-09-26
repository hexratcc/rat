// expect: 0
// output:
//| 0
//| 1
//| 9223372036854775807
//| 9223372036854775808
//| 9223372036854775811
//| 10000000000000000000
//| 18446744073709551615
#include <stdint.h>
#include <stdio.h>

int64_t Max = INT64_MAX;
long double E = 1e19L, F = 1.9L;

int main(void) {
	long double a = Max;			 // 2^63 - 1
	long double b = a + 1;		 // 2^63
	long double c = b + 3;		 // 2^63 + 3
	long double d = b * 2 - 1; // 2^64 - 1
	long double v[] = {0, F, a, b, c, E, d};
	for(int i = 0; i < 7; i++)
		printf("%llu\n", (unsigned long long)(uint64_t)v[i]);
	return 0;
}
