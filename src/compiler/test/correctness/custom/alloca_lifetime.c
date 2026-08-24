// expect: 0
extern void *alloca(__SIZE_TYPE__);

volatile int n = 64;

int check(char *p, int fill, int len) {
	int i;
	for (i = 0; i < len; i = i + 1)
		if (p[i] != fill)
			return 0;
	return 1;
}

int main(void) {
	char *a;
	char *b;
	int i;
	{
		a = __builtin_alloca(n);
		for (i = 0; i < n; i = i + 1)
			a[i] = 'a';
	}
	{
		b = alloca(n);
		for (i = 0; i < n; i = i + 1)
			b[i] = 'b';
	}
	// both blocks are gone, both allocations must still hold their bytes
	if (!check(a, 'a', n))
		return 1;
	if (!check(b, 'b', n))
		return 2;
	if (a == b)
		return 3;
	// 16-byte aligned, like gcc's alloca
	if (((unsigned long)a & 15u) || ((unsigned long)b & 15u))
		return 4;
	return 0;
}
