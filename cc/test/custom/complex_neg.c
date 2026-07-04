// expect: 12
// Unary negation of a complex value negates both parts; conversion of a complex
// value to a real type yields the real part (C99 6.3.1.7p2).
int main(void) {
	double _Complex z;
	__real__ z = 5.0;
	__imag__ z = 7.0;
	double _Complex n = -z; // -5 - 7i
	int re = (int)__real__ n;
	int im = (int)__imag__ n;
	double real = (double)z; // 5 (imaginary discarded)
	return (re + 5) * 1 + (im + 7) * 1 + (int)real * 2 + 2; // 0 + 0 + 10 + 2 = 12
}
