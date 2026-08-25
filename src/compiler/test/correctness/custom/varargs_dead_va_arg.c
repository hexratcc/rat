// expect: 0
// passes: simplifycfg

#include <stdarg.h>

long r1, r2;

void f(int i, ...) {
	va_list ap;
	va_start(ap, i);
	va_arg(ap, long); // discarded
	r1 = va_arg(ap, long);
	va_arg(ap, long); // discarded
	r2 = va_arg(ap, long);
	va_end(ap);
}

int main(void) {
	f(7, 12L, 34L, 56L, 78L);
	if(r1 != 34)
		return 1;
	if(r2 != 78)
		return 2;
	return 0;
}
