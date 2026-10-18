// expect: 0
// the stack restore closing a VLA scope after a guarded run reads the post-run state, so it must
// sit below the guard; above it, the next scope's VLA reuses v's slot and w[0] = 7 lands before
// v[0] is read
unsigned H[96];

#define L(i) c[i] = a[i] + b[i];
#define RUN8 L(0) L(1) L(2) L(3) L(4) L(5) L(6) L(7)

__attribute__((noinline)) unsigned k(unsigned *c, unsigned *a, unsigned *b, int n) {
	unsigned r = 0;
	{
		unsigned v[n];
		v[0] = 5;
		RUN8
		r += v[0];
	}
	{
		unsigned w[n];
		w[0] = 7;
		RUN8
		r += w[0];
	}
	return r;
}

int main(void) {
	return k(H + 16, H + 32, H + 64, 5) != 12;
}
