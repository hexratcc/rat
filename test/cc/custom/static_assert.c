// expect: 0
// _Static_assert (C11 6.7.10): the constant expression is checked at compile
// time and produces no code. It is valid at file scope and block scope. These
// all hold, so the program compiles and returns 0.
#include <limits.h>

_Static_assert(sizeof(int) >= 2, "int is at least 2 bytes");
_Static_assert(1 + 1 == 2, "arithmetic constant expression");

enum { N = 8 };
_Static_assert(N * N == 64, "enum constants are usable in constant expressions");

int main(void) {
	_Static_assert(sizeof(char) == 1, "char is one byte by definition");
	_Static_assert(CHAR_BIT >= 8, "a byte has at least 8 bits");
	return 0;
}
