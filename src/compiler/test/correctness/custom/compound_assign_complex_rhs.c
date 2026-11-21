// expect: 0

// a real target with a complex operand computes in complex, then keeps the real part
int main(void) {
	double d = 1.0;
	double _Complex z = 2.0;
	d += z;
	int i = 3;
	i *= z;
	return d != 3.0 || i != 6;
}
