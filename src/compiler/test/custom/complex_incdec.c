// expect: 67
// C99 6.5.2.4/6.5.3.1: ++/-- on a complex object add/subtract (1 + 0i),
// affecting only the real part. Postfix yields the prior value.
int main(void) {
	double _Complex z;
	__real__ z = 5.0;
	__imag__ z = 3.0;
	double _Complex a = z++; // a = 5+3i, z = 6+3i
	double _Complex b = ++z; // b = 7+3i, z = 7+3i
	int ar = (int)__real__ a; // 5
	int br = (int)__real__ b; // 7
	int zr = (int)__real__ z; // 7
	int zi = (int)__imag__ z; // 3 (unchanged)
	return ar * 10 + br + zr + zi; // 50 + 7 + 7 + 3 = 67
}
