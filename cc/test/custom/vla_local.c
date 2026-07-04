// expect: 100
// A 1-D variable-length array (C99 6.7.5.2): the bound `n` is a runtime value,
// so the array is stack-allocated with allocVLA. Fill it, sum it, and check
// that sizeof yields the runtime byte count (n * sizeof(int)).
int main(void) {
	int n = 5;
	int a[n];
	int i = 0;
	while (i < n) {
		a[i] = i * 10;
		i = i + 1;
	}
	int sum = 0;
	i = 0;
	while (i < n) {
		sum = sum + a[i];
		i = i + 1;
	}
	// sizeof(a) is the runtime size: n * 4 == 20 for n == 5.
	if (sizeof(a) != (unsigned long)n * sizeof(int))
		return 1;
	return sum;
}
