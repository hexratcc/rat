// expect: 0

extern void abort(void);

__attribute__((noinline)) int deref(int x, int y, int* z) {
	int a = 0, b = 0, c, d;
	for(d = 0; d < y; d++) {
		if(z)
			b = d * *z; // must stay under the guard
		for(c = 0; c < x; c++)
			a += b;
	}
	return a;
}

volatile int one = 1;
volatile int zero = 0;

__attribute__((noinline)) int divide(int n) {
	int i, r = 0;
	int num = one, den = zero;
	for(i = 0; i < n; i++) {
		if(i > 1000)
			r += num / den; // never runs, so it must never be evaluated
		r += i;
	}
	return r;
}

int main(void) {
	if(deref(3, 2, 0) != 0)
		return 1;
	if(divide(4) != 6)
		return 2;
	return 0;
}
