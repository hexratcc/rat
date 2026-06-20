// expect: 16
// Designated array initializers, where a positional element continues from
// the last designated index, and an earlier index is set afterwards.
int main(void) {
	int a[5] = { [2] = 7, 8, [0] = 1 };
	return a[0] + a[1] + a[2] + a[3] + a[4];
}
