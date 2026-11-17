// expect: 0
// The inner dimensions of a variable length array are taken at its declaration.
int main(void) {
	int n = 4;
	int a[n][n];
	for(int i = 0; i < 4; i++)
		for(int j = 0; j < 4; j++)
			a[i][j] = i * 4 + j;
	n = 10;
	if(sizeof a[0] != 4 * sizeof(int) || sizeof a != 16 * sizeof(int))
		return 1;
	if((char*)a[1] - (char*)a[0] != 4 * sizeof(int))
		return 2;
	return a[3][3] != 15 || ((int*)a)[7] != 7;
}
