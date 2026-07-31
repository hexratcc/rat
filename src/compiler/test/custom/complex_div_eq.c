// expect: 7
// Complex division (C99 6.5.5) and equality (C99 6.5.9 compares both parts).
// (8 + 6i) / (2 + 0i) = 4 + 3i. Also a real value converts to (value + 0i).
int main(void) {
	double _Complex a;
	__real__ a = 8.0;
	__imag__ a = 6.0;
	double _Complex b;
	__real__ b = 2.0;
	__imag__ b = 0.0;
	double _Complex q = a / b; // 4 + 3i
	int re = (int)__real__ q;	 // 4
	int im = (int)__imag__ q;	 // 3
	double _Complex r = 4.0;	 // 4 + 0i
	int eq = (q == r);				 // imaginary differs -> 0
	int ne = (q != r);				 // -> 1
	return re * 1 + im * 1 + eq * 100 + ne * 0; // 4 + 3 + 0 = 7
}
