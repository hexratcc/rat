// expect: 0
// a guarded pack may only speculate loads that read the run's incoming memory
// state
unsigned C[32], B[32];

__attribute__((noinline)) void k(unsigned *c, unsigned *a, unsigned *b) {
	unsigned t0 = a[0], t1 = a[1], t2 = a[2], t3 = a[3];
	unsigned t4 = a[4], t5 = a[5], t6 = a[6], t7 = a[7];
	c[8] = 7; // clobbers a[0] when a == c + 8, and is not part of the run
	c[0] = t0 + b[0];
	c[1] = t1 + b[1];
	c[2] = t2 + b[2];
	c[3] = t3 + b[3];
	c[4] = t4 + b[4];
	c[5] = t5 + b[5];
	c[6] = t6 + b[6];
	c[7] = t7 + b[7];
}

int main(void) {
	for(int i = 0; i < 32; i++) {
		C[i] = 100 + i;
		B[i] = 1000 + i;
	}
	k(C, C + 8, B); // guard passes: vector arm
	if(C[0] != 1108)
		return 1;
	for(int i = 0; i < 32; i++)
		C[i] = 100 + i;
	k(C, C + 8, C); // self-aliased: scalar arm
	if(C[0] != 208)
		return 2;
	return 0;
}
