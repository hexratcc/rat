// expect: 0
// A constant typedef bound is folded where the typedef is, a later local of the same name does
// not change it.
long v;
typedef int T[sizeof v];
T g;

int main(void) {
	char v = 0;
	T x = {1, 2};
	typedef char U[sizeof v + 1];
	{
		long v = 0;
		U y = {3};
		if(sizeof y != 2 || y[0] != 3 || v != 0)
			return 1;
	}
	if(sizeof x != sizeof(long) * sizeof(int) || sizeof g != sizeof x)
		return 2;
	return x[0] + x[1] - 3 + v;
}
