// expect: 0
// A later declaration completes an array first declared without a size.
int g[];
int g[5];
extern int e[];
int e[3] = {1, 2, 3};

int main(void) {
	g[4] = 3;
	if(sizeof g != 5 * sizeof(int) || sizeof e != 3 * sizeof(int))
		return 1;
	return g[4] + e[2] - 6;
}
