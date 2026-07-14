// expect: 63
// a static helper is inlined into its only caller and then removed as dead by
// the dead-function-elimination pass; the result is unchanged
static int triple(int a) { return a * 3; }
static int also_unused(int a) { return a + 100; }
int main(void) {
	return triple(21);
}
