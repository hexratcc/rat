// expect: 0
// r = &p[i + 1] - i is &p[1]: the index terms cancel, so r's group must keep i, else the else arm's
// wide store of p takes r's pointer as the group anchor and computes it from i, a load of the then arm
int P[64], X[16];

__attribute__((noinline)) void f(int *p, int *x, int *idx, int c, long junk) {
	int x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
	if(c) {
		int i = *idx;
		int *r = &p[i + 1] - i;
		r[-1] = x0 * 3 + 1;
		r[0] = x1 * 3 + 1;
		r[1] = x2 * 3 + 1;
		r[2] = x3 * 3 + 1;
	} else {
		p[0] = x0 * 5 + 2;
		p[1] = x1 * 5 + 2;
		p[2] = x2 * 5 + 2;
		p[3] = x3 * 5 + 2;
	}
}

int main(void) {
	static const int want[12] = {4, 7, 10, 13, 0, 0, 0, 0, 7, 12, 17, 22};
	int idx = 7;
	for(int k = 0; k < 16; k++)
		X[k] = k + 1;
	f(P + 8, X, &idx, 1, 0x100000000L);
	f(P + 16, X, &idx, 0, 0x100000000L);
	for(int k = 0; k < 12; k++)
		if(P[8 + k] != want[k])
			return 1;
	return 0;
}
