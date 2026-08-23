// expect: 0
// passes: gvn,slp,memoryopt

struct S {
	long double l;
	unsigned long long k;
};

struct S g;
long double want;

void init(void) {
	g.l = 5.25L;
	g.k = 7;
	want = 5.25L;
}

int main(void) {
	struct S t;
	init();
	t = g; // slp fuses the first two 8-byte copy lanes into one <2 x i64> load
	if (t.k != 7)
		return 2;
	if (g.l != want) // this f128 load must not be replaced by that vector load
		return 1;
	return 0;
}
