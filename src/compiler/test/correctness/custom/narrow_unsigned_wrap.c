// expect: 0
// (int)(u + 1u) wraps for u = INT_MAX (unsigned sum, then a conversion modulo 2^32), so the fourth
// store goes to p[INT_MIN], 2^32 elements below the other three. Only a signed sum may split as
// sext(u) + 1. p sits in the middle of a 32 GiB reservation, so both ends are mapped
#include <limits.h>
#include <sys/mman.h>

__attribute__((noinline)) void f(int* p, unsigned u, int* x) {
	int a = x[0];
	int b = x[1];
	int c = x[2];
	int d = x[3];
	p[(int)u - 2] = a * 3 + 1;
	p[(int)u - 1] = b * 3 + 1;
	p[(int)u] = c * 3 + 1;
	p[(int)(u + 1u)] = d * 3 + 1;
}

int main(void) {
	unsigned long size = 1ul << 35;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	char* m = mmap(0, size, prot, flags, -1, 0);
	if(m == MAP_FAILED)
		return 2;
	int* p = (int*)(m + (1ul << 34));
	int x[4] = {1, 2, 3, 4};
	f(p, INT_MAX, x);
	if(p[INT_MAX - 2] != 4 || p[INT_MAX - 1] != 7 || p[INT_MAX] != 10)
		return 1;
	return p[INT_MIN] != 13 || p[(long)INT_MAX + 1] != 0;
}
