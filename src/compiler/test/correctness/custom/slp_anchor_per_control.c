// expect: 0
// p + 4 * (i + 1) and p + 4 * i share an address group only where i + 1 does not overflow. With
// i = INT_MAX only the else arm runs, so its wide load must not take the then arm's p[i + 1] as its
// anchor. p sits in a 16 GiB reservation, so p[INT_MAX] is mapped
#include <limits.h>
#include <sys/mman.h>

__attribute__((noinline)) void f(int* restrict p, int i, int c, int* restrict s) {
	if(c) {
		s[0] = p[i + 1];
		s[1] = p[i + 2];
		s[2] = p[i + 3];
		s[3] = p[i + 4];
	} else {
		s[0] = p[i - 3];
		s[1] = p[i - 2];
		s[2] = p[i - 1];
		s[3] = p[i];
	}
}

int main(void) {
	unsigned long size = 1ul << 34;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	char* m = mmap(0, size, prot, flags, -1, 0);
	if(m == MAP_FAILED)
		return 2;
	int* p = (int*)m;
	for(int k = 0; k < 4; ++k)
		p[INT_MAX - 3 + k] = 10 + k;
	int s[4] = {0, 0, 0, 0};
	f(p, INT_MAX, 0, s);
	return s[0] != 10 || s[1] != 11 || s[2] != 12 || s[3] != 13;
}
