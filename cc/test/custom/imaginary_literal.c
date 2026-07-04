// expect: 12
// Imaginary floating constants (C99 6.4.4.2 / Annex G): a constant with an
// 'i'/'I'/'j'/'J' suffix is a complex value whose real part is zero and whose
// imaginary part is the written value. A size suffix may be combined with it.
// Parts are read with the __real__/__imag__ operators (C99 6.2.5p13) so the
// test needs no <complex.h> library calls.

int main(void) {
	double _Complex a = 3.0i;	// 0 + 3i
	double _Complex b = 4.0i;	// 0 + 4i
	double _Complex c = a * b;	// (3i)(4i) = -12 + 0i
	float _Complex d = 2.0fi;	// size suffix + imaginary suffix combined
	int im = (int)(__imag__ a + __imag__ b);	// 3 + 4 = 7
	int re = (int)__real__ c;			// -12
	int di = (int)__imag__ d;			// 2
	return im - re - di - 5;				// 7 + 12 - 2 - 5 = 12
}
