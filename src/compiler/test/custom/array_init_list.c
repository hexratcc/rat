// expect: 60
// A brace-enclosed initializer list for a sized-by-inference int array.
int main(void) {
	int a[] = {10, 20, 30};
	return a[0] + a[1] + a[2];
}
