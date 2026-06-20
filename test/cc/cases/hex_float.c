// expect: 14
// Hexadecimal floating constants (C99 6.4.4.2).
int main(void) {
	double a = 0x1.8p3; // 1.5 * 2^3   = 12.0
	double b = 0x1p0;		// 1.0 * 2^0   = 1.0
	double c = 0x0.8p1; // 0.5 * 2^1   = 1.0
	return (int)(a + b + c);
}
