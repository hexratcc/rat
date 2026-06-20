// expect: 42

// extern (prototype-only) libc functions: malloc returns void*, which must
// not be truncated through an implicit int declaration.
void *malloc(unsigned long n);
void free(void *p);

int main(void) {
	int *a = (int *)malloc(sizeof(int) * 4);
	a[0] = 10;
	a[1] = 20;
	a[2] = 5;
	a[3] = 7;
	int s = 0;
	for (int i = 0; i < 4; ++i)
		s += a[i];
	free(a);
	return s;
}
