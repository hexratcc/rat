// expect: 0
// passes:

#include <stdarg.h>

typedef int Row[4];

struct S {
	va_list ap;
	Row row;
};

int f(int i, ...) {
	struct S s;
	int a, b;
	va_start(s.ap, i);
	a = va_arg(s.ap, int);
	b = va_arg(s.ap, int);
	va_end(s.ap);
	return a + b;
}

int main(void) {
	struct S s;
	if(f(1, 40, 2) != 42)
		return 1;
	if(sizeof s.row != 4 * sizeof(int))
		return 2;
	s.row[2] = 7;
	if(*(s.row + 2) != 7)
		return 3;
	return 0;
}
