// expect: 34
// GNU __real__/__imag__ as lvalues addressing the sub-objects of a complex
// value (C99 6.2.5: a complex value is laid out as { real, imaginary }).
int main(void) {
	double _Complex z;
	__real__ z = 3.0;
	__imag__ z = 4.0;
	int re = (int)__real__ z;
	int im = (int)__imag__ z;
	return re * 10 + im; // 34
}
