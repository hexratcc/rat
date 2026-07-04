// expect: 3
// Pointer minus pointer yields the element count between them.
int main(void) {
	int a[5];
	int *p = &a[1];
	int *q = &a[4];
	return q - p;
}
