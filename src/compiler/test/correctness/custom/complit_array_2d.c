// expect: 0
// A 2D compound literal gets storage for all its rows.
// Several live ones expose an under-allocated object.
static int sum(int (*m)[3]) {
	int s = 0;
	for(int i = 0; i < 2; i++)
		for(int j = 0; j < 3; j++)
			s = s * 10 + m[i][j];
	return s;
}

int main(void) {
	int (*a)[3] = (int[2][3]){{1, 2, 3}, {4, 5, 6}};
	int (*b)[3] = (int[2][3]){{7, 8, 9}, {1, 2, 3}};
	int (*c)[3] = (int[2][3]){{4, 5, 6}, {7, 8, 9}};
	if(sum(a) != 123456)
		return 1;
	if(sum(b) != 789123)
		return 2;
	if(sum(c) != 456789)
		return 3;
	return 0;
}
