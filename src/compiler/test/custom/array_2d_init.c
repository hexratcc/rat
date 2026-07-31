// expect: 41
// A two-dimensional array with a nested brace initializer.
int main(void) {
	int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
	return m[0][0] * 1 + m[0][1] * 2 + m[1][2] * 6;
}
