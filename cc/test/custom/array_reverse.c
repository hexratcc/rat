// expect: 4
// Write a descending pattern, then read back through a pointer.
int main(void) {
	int a[5];
	int i = 0;
	while (i < 5) {
		a[i] = 5 - i;
		i = i + 1;
	}
	int *p = &a[1];
	return *p;
}
