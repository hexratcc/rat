// expect: 0
// a wide load packed for an earlier processed run reads an inner state of a later run: the inlined
// stores to c are created after the caller's stores to d, so the d run packs first and its wide
// load of p reads the state after c[1]; the c run must see that load as a reader and stay scalar
unsigned D[4], H[8];

__attribute__((noinline)) void f(void) {}

static inline void lo2(unsigned *c, unsigned y) {
	c[0] = y + 1;
	c[1] = y + 2;
}

static inline void hi2(unsigned *c, unsigned y) {
	c[2] = y + 3;
	c[3] = y + 4;
}

__attribute__((noinline)) void k(unsigned *c, unsigned *p, unsigned y) {
	lo2(c, y);
	unsigned x0 = p[0], x1 = p[1], x2 = p[2], x3 = p[3];
	hi2(c, y);
	f();
	D[0] = x0;
	D[1] = x1;
	D[2] = x2;
	D[3] = x3;
}

int main(void) {
	for(int i = 0; i < 8; i++)
		H[i] = 100 + i;
	k(H, H, 10); // p[0], p[1] read the new c[0], c[1]
	if(D[0] != 11 || D[1] != 12 || D[2] != 102 || D[3] != 103)
		return 1;
	k(H, H + 4, 20);
	if(D[0] != 104 || D[1] != 105 || D[2] != 106 || D[3] != 107 || H[3] != 24)
		return 2;
	return 0;
}
