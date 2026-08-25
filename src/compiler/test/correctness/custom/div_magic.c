// expect: 152
// division and remainder by non-power-of-two constants must survive the
// magic-number strength reduction; inputs derive from argc so they are
// unknown until runtime. Expected value cross-checked against gcc -O2.
int main(int argc, char** argv) {
	(void)argv;
	int a = argc * 100003;						 // 100003, unknown until runtime
	int n = argc * -98765;						 // -98765
	unsigned u = (unsigned)argc * 3000000007u; // 3000000007
	int int_min = (int)((unsigned)argc << 31); // INT_MIN
	int r = 0;
	r += a / 7;					// 14286
	r += a % 7;					// 1
	r += a / -7;				// -14286
	r += a % -7;				// 1
	r += n / 7;					// -14109
	r += n % 7;					// -2
	r += n / -641;			// 154
	r += n % -641;			// -51
	r += a / 10;				// 10000
	r += a % 10;				// 3
	r += a / 3;					// 33334
	r += a / 6;					// 16667
	r += a / 4;					// 25000 (signed pow2)
	r += n / 4;					// -24691 (signed pow2, negative dividend)
	r += n % 4;					// -1
	r += (int)(u / 7u);		// 428571429
	r += (int)(u % 7u);		// 4
	r += (int)(u / 10u);	// 300000000
	r += (int)(u % 10u);	// 7
	r += (int)(u / 641u);	// 4680188
	r += (int)(u % 641u);	// 499
	r += (int)(u / 2147483649u); // 1 (divisor > 2^31)
	r += int_min / 7;			// -306783378
	r += int_min % 7;			// -2
	r += int_min / -7;		// 306783378
	return r & 255;
}
