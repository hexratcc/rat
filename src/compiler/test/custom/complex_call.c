// expect: 46
// Complex values passed and returned by value (the by-reference aggregate ABI).
double _Complex cadd(double _Complex x, double _Complex y) { return x + y; }

double _Complex cmake(double r, double i) {
	double _Complex z;
	__real__ z = r;
	__imag__ z = i;
	return z;
}

int main(void) {
	double _Complex a = cmake(1.0, 2.0);
	double _Complex b = cmake(3.0, 4.0);
	double _Complex c = cadd(a, b); // (4) + (6)i
	int re = (int)__real__ c;
	int im = (int)__imag__ c;
	return re * 10 + im; // 46
}
