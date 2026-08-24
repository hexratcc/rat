// expect: 0

int a[4] = {1, 2, 3, 4};
extern int b[4] __attribute__((alias("a")));

int zeros[3];
extern int z[3] __attribute__((alias("zeros")));

int idx;

int main(void) {
	int i;
	for(i = 0; i < 4; ++i)
		if(b[i] != a[i])
			return 1;

	b[idx] = 9;
	if(a[0] != 9)
		return 2;

	idx = 3;
	a[idx] = 7;
	if(b[3] != 7)
		return 3;

	// a bss target works the same way
	z[1] = 5;
	if(zeros[1] != 5 || zeros[0] != 0 || zeros[2] != 0)
		return 4;
	return 0;
}
