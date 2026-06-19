// expect: 21
// A two-dimensional array: element-by-element assignment and read-back.
int main(void) {
	int m[2][3];
	int i = 0;
	int j = 0;
	int v = 1;
	for (i = 0; i < 2; i = i + 1)
		for (j = 0; j < 3; j = j + 1) {
			m[i][j] = v;
			v = v + 1;
		}
	int s = 0;
	for (i = 0; i < 2; i = i + 1)
		for (j = 0; j < 3; j = j + 1)
			s = s + m[i][j];
	return s;
}
