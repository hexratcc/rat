// expect: 0
// The size expression of a variable length array typedef runs once, at the typedef.
static int calls;

static int bound(int n) {
	calls++;
	return n;
}

int main(void) {
	typedef int T[bound(5)];
	if(calls != 1)
		return 1;
	T a;
	T b;
	a[4] = 1;
	b[4] = 2;
	if(calls != 1)
		return 2;
	if(sizeof a != 5 * sizeof(int) || sizeof b != 5 * sizeof(int))
		return 3;
	return a[4] + b[4] - 3;
}
