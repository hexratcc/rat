// expect: 44
// C99 6.7.5.2: a multi-dimensional variable-length array `a[n][n]` is laid out
// at run time. Both the allocation and subscripting must scale by the runtime
// row stride (n * sizeof(int)), since a pointer to the inner VLA has a size
// known only at run time.
int main(void) {
	int n = 4;
	int a[n][n];
	for (int i = 0; i < n; i++)
		for (int j = 0; j < n; j++)
			a[i][j] = i * n + j;
	int s = 0;
	for (int i = 0; i < n; i++)
		s += a[i][i]; // diagonal: 0 + 5 + 10 + 15 = 30
	return s + a[3][2]; // 30 + 14 = 44
}
