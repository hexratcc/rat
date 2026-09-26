// expect: 0
// output:
//| 7 7 7 7 7 7 0xap-3
//| 7 7 7 7 7 7 7 0xap-3 7 0xap-3
//| 43.25
#include <stdarg.h>
#include <stdio.h>

long double L = 1.25L;
long long I = 7;

long double sum(int n, ...) {
	va_list ap;
	va_start(ap, n);
	long double r = 0;
	for(int i = 0; i < n; i++)
		r += va_arg(ap, long long);
	r += va_arg(ap, long double);
	va_end(ap);
	return r;
}

int main(void) {
	printf("%lld %lld %lld %lld %lld %lld %La\n", I, I, I, I, I, I, L);
	printf("%lld %lld %lld %lld %lld %lld %lld %La %lld %La\n", I, I, I, I, I, I, I, L, I, L);
	printf("%Lg\n", sum(6, I, I, I, I, I, I, L));
	return 0;
}
