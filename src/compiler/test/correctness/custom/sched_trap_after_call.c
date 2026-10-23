// expect: 0
// foo exits before the division by zero. The wide load of a0..a3 must run before foo, and the
// scheduler must not let the division move above foo while foo waits for that load
#include <stdlib.h>

unsigned A[4] = {1, 2, 3, 4}, B[4];
volatile int zero = 0;

__attribute__((noinline)) void foo(int e) {
	if(e)
		exit(0);
}

__attribute__((noinline)) int f(int p, int q, int* r) {
	unsigned a0 = A[0], a1 = A[1], a2 = A[2], a3 = A[3];
	foo(q == 0);
	int x = p / q;
	B[0] = a0;
	B[1] = a1;
	B[2] = a2;
	B[3] = a3;
	*r = x;
	return 0;
}

int main(void) {
	int r;
	f(7, zero, &r);
	return 1;
}
