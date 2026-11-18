// expect: 0
// A typedef of a variable length array takes its size at run time.
static int f(int n) {
	typedef int T[n];
	T t;
	for(int i = 0; i < n; i++)
		t[i] = i;
	if(sizeof t != n * sizeof(int))
		return 1;
	return t[n - 1] != n - 1;
}

int main(void) {
	return f(5);
}
