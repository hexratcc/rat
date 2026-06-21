// expect: 3
// A partial initializer list zero-fills the remaining elements.
int main(void) {
	int a[5] = {1, 2};
	return a[0] + a[1] + a[2] + a[3] + a[4];
}
