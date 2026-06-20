// expect: 1
// C99 6.5.3.4: sizeof applied to an array type-name, including multi-dimensional.
int main(void) {
	int a = sizeof(int[4]) == 4 * sizeof(int);
	int b = sizeof(char[3][5]) == 15;
	int c = sizeof(int[2][4]) == 8 * sizeof(int);
	int d = sizeof(int *[4]) == 4 * sizeof(int *);
	return a && b && c && d;
}
