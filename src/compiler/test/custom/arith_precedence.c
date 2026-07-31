// expect: 12
// Multiplicative binds tighter than additive: 2 + 12 - 2.
int main(void) {
	return 2 + 3 * 4 - 10 / 5;
}
