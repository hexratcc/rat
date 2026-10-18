// expect: 0
// a restrict parameter that is assigned loses the restrict fact: after p = q, the loads through p
// are based on p, so they may read what the stores through o (p's first value) wrote
unsigned H[8];

__attribute__((noinline)) void k(unsigned* restrict p, unsigned* q) {
	unsigned* o = p;
	p = q;
	o[0] = p[0] + 1;
	o[1] = p[1] + 1;
	o[2] = p[2] + 1;
	o[3] = p[3] + 1;
}

// the assignment may sit in a _Generic association or an array bound: every evaluated one counts
__attribute__((noinline)) void g(int* restrict p, int* q) {
	int* o = p;
	_Generic(0, int: p = q);
	p[1] = 5;
	p[0] = o[1] + 1;
}

__attribute__((noinline)) void v(int* restrict p, int* q, int n) {
	int* o = p;
	int t[(p = q, n)];
	t[0] = 0;
	p[1] = 5 + t[0];
	p[0] = o[1] + 1;
}

int main(void) {
	for(int i = 0; i < 8; i++)
		H[i] = 100 + 7 * i;
	k(H + 1, H); // each p[i] reads the o[i - 1] just stored
	for(int i = 1; i < 5; i++)
		if(H[i] != (unsigned)(100 + i))
			return i;
	int a[2] = {10, 20};
	g(a, a);
	if(a[0] != 6 || a[1] != 5)
		return 10;
	a[1] = 20;
	v(a, a, 1);
	if(a[0] != 6 || a[1] != 5)
		return 11;
	return 0;
}
