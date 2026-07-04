// expect: 30
// Pass a function pointer as a parameter and call it indirectly.
int mul(int a, int b) { return a * b; }
int apply(int (*op)(int, int), int x, int y) { return op(x, y); }
int main(void) {
	return apply(mul, 6, 5);
}
