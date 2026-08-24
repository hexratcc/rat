// expect: 1
// Pointer comparison: a pointer past the base is greater than the base.
int main(void) {
	int a[3];
	int *p = a;
	int *q = a + 2;
	return q > p;
}
