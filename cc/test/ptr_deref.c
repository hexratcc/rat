// expect: 42
// Address-of a local and write through the pointer.
int main(void) {
	int x = 41;
	int *p = &x;
	*p = *p + 1;
	return x;
}
