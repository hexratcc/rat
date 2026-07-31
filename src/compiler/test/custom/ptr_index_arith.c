// expect: 30
// Index through a pointer offset: p[2] reads a[3].
int main(void) {
	int a[5];
	a[3] = 30;
	int *p = a + 1;
	return p[2];
}
