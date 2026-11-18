// expect: 0
// A variable dimension below constant ones still makes the array variable length.
int main(void) {
	int k = 4;
	int a[2][3][k];
	int guard = 5;
	for(int i = 0; i < 2; i++)
		for(int j = 0; j < 3; j++)
			for(int l = 0; l < k; l++)
				a[i][j][l] = i * 100 + j * 10 + l;
	if(sizeof a != 2 * 3 * 4 * sizeof(int))
		return 1;
	if(a[0][0][0] != 0 || a[1][2][3] != 123 || guard != 5)
		return 2;
	int sized[sizeof(int[3][k])];
	if(sizeof sized != 3 * 4 * sizeof(int) * sizeof(int))
		return 3;
	return 0;
}
