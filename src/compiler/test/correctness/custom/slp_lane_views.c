// expect: 0
// lanes of different opcodes pack as one or two vector ops when each is viewed as a binary op: ~x as
// x ^ -1, -x as 0 - x, x - c as x + -c, at the wrap of the 32-bit range (unsigned, so C defines it)
unsigned A[4], B[4], C[4], D[4], E[4];

__attribute__((noinline)) void views(unsigned* restrict c, unsigned* restrict d, unsigned* restrict e,
		const unsigned* restrict a, const unsigned* restrict b) {
	c[0] = a[0] ^ b[0];
	c[1] = ~a[1];
	c[2] = a[2] ^ b[2];
	c[3] = ~a[3];
	d[0] = a[0] - b[0];
	d[1] = -a[1];
	d[2] = a[2] - b[2];
	d[3] = -a[3];
	e[0] = a[0] + b[0];
	e[1] = a[1] - 7;
	e[2] = a[2] - 0x80000000u;
	e[3] = -a[3];
}

int main(void) {
	unsigned av[4] = {5, 0x80000000u, 0x7fffffffu, 0xfffffffdu};
	unsigned bv[4] = {9, 1, 0xffffffffu, 12};
	for(int k = 0; k < 4; k++) {
		A[k] = av[k];
		B[k] = bv[k];
	}
	views(C, D, E, A, B);
	for(int k = 0; k < 4; k++) {
		if(C[k] != (k % 2 ? ~A[k] : A[k] ^ B[k]))
			return 1 + k;
		if(D[k] != (k % 2 ? 0u - A[k] : A[k] - B[k]))
			return 5 + k;
	}
	if(E[0] != 14 || E[1] != A[1] - 7u || E[2] != A[2] + 0x80000000u || E[3] != 3)
		return 9;
	return 0;
}
