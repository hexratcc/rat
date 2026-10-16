// expect: 0
// the load of p stays below the store to restrict r: its address uses a VLA allocated after that
// store, and the scheduler, which does not separate r from p, orders a hoisted load before the store
// (a cycle in the block's schedule)
__attribute__((noinline)) long f(long *restrict r, long *p, int n) {
	r[0] = 5;
	long vla[n];
	return p[(unsigned long)vla >> 60];
}

int main(void) {
	long a[4] = {0, 0, 0, 0}, b[4] = {7, 8, 9, 10};
	if(f(a, b, 3) != 7 || a[0] != 5)
		return 1;
	return 0;
}
