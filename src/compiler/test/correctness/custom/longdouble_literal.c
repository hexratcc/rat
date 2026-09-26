// expect: 0
// output:
//| 0xa.5a1cac083126e98p+1 -0xc.ccccccccccccccdp-7
//| 0x8.000000000000001p-3 0xd.1ba8323fe558c61p+13284
//| 0x9.99999999999999ap-5
#include <stdio.h>

int main(void) {
	long double a = 20.704L;
	long double b = -0.1L;
	long double c = 0x1.0000000000000002p0L;
	long double d = 1e4000L;
	long double _Complex z = 0.3Li;
	printf("%La %La\n", a, b);
	printf("%La %La\n", c, d);
	printf("%La\n", __imag__ z);
	return 0;
}
