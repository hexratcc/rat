// expect: 66

// 'restrict' is accepted as a type qualifier on declarations and at the
// pointer level; array-parameter qualifiers (static/const/restrict) decay
// the parameter to a pointer.
int sum(const int *restrict a, int b[static 3], int n) {
	int total = 0;
	for (int i = 0; i < n; ++i)
		total += a[i] + b[i];
	return total;
}

int main(void) {
	int xs[3] = {1, 2, 3};
	int ys[3] = {10, 20, 30};
	int *restrict p = xs;
	return sum(p, ys, 3);
}
