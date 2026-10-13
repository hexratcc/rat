// expect: 0
// a pointer whose offset is computed from another pointer is based on that one (C11 6.7.3.1p3):
// a + (d - a) is d, so its accesses may alias the ones through restrict d, also when the
// difference goes through memory or a callee, and d + (e - d) is e
int G[8];

// the load through x stays below the store through d
__attribute__((noinline)) void load(int* restrict d, int* a) {
	int* x = a + (d - a);
	d[0] = 1;
	d[1] = x[0];
}

// a packed run on d whose lanes read through x
__attribute__((noinline)) void run(int* restrict d, int* a) {
	int* x = a + (d - a);
	d[1] = x[0];
	d[2] = x[1];
	d[3] = x[2];
	d[4] = x[3];
}

// y is a, not d: its load stays below the store through a
__attribute__((noinline)) void reverse(int* restrict d, int* a) {
	int* y = d + (a - d);
	a[0] = 1;
	d[0] = y[0];
}

// y is e: the scheduler keeps its load above the store through e
__attribute__((noinline)) int sched(int* restrict d, int* restrict e) {
	int* y = d + (e - d);
	int v = y[0];
	e[0] = 1;
	return v * 3 + e[1];
}

// an object plus a difference to d is d
__attribute__((noinline)) void object(int* restrict d) {
	int* x = G + (d - G);
	d[0] = 1;
	d[1] = x[0];
}

// the difference read back from memory
__attribute__((noinline)) void memory(int* restrict d, int* a, long* t) {
	*t = d - a;
	d[0] = 1;
	int* x = a + *t;
	d[1] = x[0];
}

__attribute__((noinline)) void diff(long* t, int* d, int* a) {
	*t = d - a;
}

// the difference stored by a callee that takes d
__attribute__((noinline)) void call(int* restrict d, int* a, long* t) {
	diff(t, d, a);
	d[0] = 1;
	int* x = a + *t;
	d[1] = x[0];
}

int main(void) {
	int bad = 0;
	int b[8] = {7, 7, 7, 7, 7, 7, 7, 7};
	load(b, b + 2);
	bad += b[1] != 1;
	int c[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	run(c, c + 3);
	bad += c[1] != 1 || c[2] != 1 || c[3] != 1 || c[4] != 1;
	int e[4] = {7, 7, 7, 7};
	reverse(e + 2, e);
	bad += e[2] != 1;
	int s[4] = {7, 7, 7, 7};
	bad += sched(s, s + 2) != 28;
	for(int i = 0; i < 8; i++)
		G[i] = 7;
	object(G + 2);
	bad += G[3] != 1;
	int m[4] = {7, 7, 7, 7};
	long t;
	memory(m, m + 2, &t);
	bad += m[1] != 1;
	int n[4] = {7, 7, 7, 7};
	call(n, n + 2, &t);
	bad += n[1] != 1;
	return bad;
}
