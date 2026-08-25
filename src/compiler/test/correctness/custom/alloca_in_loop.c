// expect: 0
volatile int n = 32;

int main(void) {
	char *seen[16];
	int i;
	int j;
	int k;
	for (i = 0; i < 16; i = i + 1) {
		char *p = __builtin_alloca(n);
		for (k = 0; k < n; k = k + 1)
			p[k] = (char)i;
		seen[i] = p;
	}
	// each allocation still holds the byte the iteration that made it wrote
	for (i = 0; i < 16; i = i + 1)
		for (j = 0; j < n; j = j + 1)
			if (seen[i][j] != (char)i)
				return 1;
	// and they are all distinct
	for (i = 0; i < 16; i = i + 1)
		for (j = i + 1; j < 16; j = j + 1)
			if (seen[i] == seen[j])
				return 2;
	return 0;
}
