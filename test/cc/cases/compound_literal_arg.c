// expect: 10
// An array compound literal passed (decayed to a pointer) as an argument.
int sum(int *a, int n) {
	int s = 0;
	int i = 0;
	for (i = 0; i < n; i = i + 1)
		s = s + a[i];
	return s;
}
int main(void) {
	return sum((int[]){1, 2, 3, 4}, 4);
}
