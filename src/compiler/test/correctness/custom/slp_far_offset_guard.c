// expect: 0
// k = i + 0x1FFFFFFFFFFFFFF0 wraps to 0, so q[k + j] is q[j] and p[0] is q[15]. The byte offsets of
// q[k + j] from q are near 2^63, so the guard range of the 16 loads must not overflow
__attribute__((noinline)) void f(int* p, int* q, unsigned long i) {
	unsigned long k = i + 0x1FFFFFFFFFFFFFF0ul;
	p[0] = q[k + 0] + 1;
	p[1] = q[k + 1] + 1;
	p[2] = q[k + 2] + 1;
	p[3] = q[k + 3] + 1;
	p[4] = q[k + 4] + 1;
	p[5] = q[k + 5] + 1;
	p[6] = q[k + 6] + 1;
	p[7] = q[k + 7] + 1;
	p[8] = q[k + 8] + 1;
	p[9] = q[k + 9] + 1;
	p[10] = q[k + 10] + 1;
	p[11] = q[k + 11] + 1;
	p[12] = q[k + 12] + 1;
	p[13] = q[k + 13] + 1;
	p[14] = q[k + 14] + 1;
	p[15] = q[k + 15] + 1;
}

int main(void) {
	int a[31];
	for(int j = 0; j < 31; ++j)
		a[j] = 100 * j;
	f(a + 15, a, 0xE000000000000010ul);
	// the store to a[15] comes before the load of q[15]
	return a[30] != 2;
}
