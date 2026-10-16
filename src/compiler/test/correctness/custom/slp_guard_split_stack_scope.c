// expect: 0
// a guarded run, then a VLA scope and a call: the stack ops and the call access states after the
// run, so the block split numbers their states before it moves any of them below the merge
unsigned G[96], H[96];

__attribute__((noinline)) void ext(unsigned *x) {
	x[2] ^= 0x99u;
}

__attribute__((noinline)) void k(unsigned *p, unsigned *q, int n, int sel) {
	if(sel & 2)
		((unsigned char *)q)[76] = (unsigned char)159;
	q[0] = (q[17] - p[0]) ^ 312u;
	q[1] = (q[18] - p[1]) ^ 312u;
	q[2] = (q[19] - p[2]) ^ 312u;
	q[3] = (q[20] - p[3]) ^ 312u;
	{
		unsigned v[n];
	}
	ext(p);
}

// the same in a loop, which SLP leaves alone
__attribute__((noinline)) void ref(unsigned *p, unsigned *q, int sel) {
	if(sel & 2)
		((unsigned char *)q)[76] = (unsigned char)159;
	for(int i = 0; i < 4; i++)
		q[i] = (q[17 + i] - p[i]) ^ 312u;
	p[2] ^= 0x99u;
}

static int check(int pa, int qa, int sel) {
	for(int i = 0; i < 96; i++)
		G[i] = H[i] = i * 7 + 1;
	k(G + pa, G + qa, 3, sel);
	ref(H + pa, H + qa, sel);
	for(int i = 0; i < 96; i++)
		if(G[i] != H[i])
			return 1;
	return 0;
}

int main(void) {
	for(int sel = 0; sel < 4; sel++) {
		if(check(0, 48, sel)) // vector arm
			return 1;
		if(check(1, 0, sel)) // scalar arm, p overlaps q
			return 2;
	}
	return 0;
}
