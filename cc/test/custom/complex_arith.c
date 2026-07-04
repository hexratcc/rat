// expect: 42
// C99 6.5.5/6.5.6: complex arithmetic. (1+2i)*(3+4i) = (3-8) + (4+6)i
// = -5 + 10i; real part -5, imaginary 10. Encode |(-5)|*4 + 10*2 = 40+... no:
// return a small integer derived from the parts: (re + 25) + imag*... keep it
// simple and exact with integral results.
int main(void) {
	double _Complex a, b;
	__real__ a = 1.0;
	__imag__ a = 2.0;
	__real__ b = 3.0;
	__imag__ b = 4.0;
	double _Complex p = a * b; // (-5) + 10i
	int re = (int)__real__ p;  // -5
	int im = (int)__imag__ p;  // 10
	return (re + 5) * 100 + im * 4 + 2; // 0 + 40 + 2 = 42
}
