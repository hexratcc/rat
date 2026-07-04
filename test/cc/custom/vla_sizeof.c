// expect: 1
// sizeof applied to a variable-length-array type name is computed at run time
// and its size-expression operands ARE evaluated for their side effects
// (C99 6.5.3.4p2). Here calling f() both sizes the array and bumps a counter.
int calls = 0;

int f(void) {
	calls++;
	return 5;
}

int main(void) {
	int n = 3;
	// sizeof(int[n]) == 3 * sizeof(int); the operand n is read at run time.
	unsigned long a = sizeof(int[n]);
	// sizeof(int[f()]) evaluates f() (side effect: calls becomes 1) and yields
	// 5 * sizeof(int).
	unsigned long b = sizeof(int[f()]);
	// 2D VLA: sizeof(int[n][f()]) evaluates f() again -> calls becomes 2.
	unsigned long c = sizeof(int[n][f()]);
	unsigned long si = sizeof(int);
	// a/si = 3, b/si = 5, c/si = 15, calls = 2.
	return (a == 3 * si) && (b == 5 * si) && (c == 15 * si) && (calls == 2);
}
