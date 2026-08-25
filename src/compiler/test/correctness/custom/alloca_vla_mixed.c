// expect: 0
volatile int n = 128;
void *volatile sink;

int main(void) {
	char *kept[8];
	char *vla[8];
	int i;
	int j;
	int k;
	for (i = 0; i < 8; i = i + 1) {
		char v[n];
		for (k = 0; k < n; k = k + 1)
			v[k] = (char)(i + 1);
		sink = v;
		vla[i] = v;
		kept[i] = __builtin_alloca(16);
		for (k = 0; k < 16; k = k + 1)
			kept[i][k] = (char)(i + 100);
	}
	// alloca storage survives every block exit and the loop itself
	for (i = 0; i < 8; i = i + 1) {
		for (k = 0; k < 16; k = k + 1)
			if (kept[i][k] != (char)(i + 100))
				return 1;
		for (j = i + 1; j < 8; j = j + 1)
			if (kept[i] == kept[j])
				return 2;
	}
	// no alloca block overlaps a VLA that was allocated after it
	for (i = 0; i < 8; i = i + 1)
		for (j = 0; j <= i; j = j + 1)
			if (kept[j] < vla[i] + n && vla[i] < kept[j] + 16)
				return 3;
	return 0;
}
