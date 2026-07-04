// expect: 14
// 1 + 6 - 2 + (2 << 1): 1+6=7, -2=5, 5%3 part already in 2, 2<<1=4 -> 5+... see below.
// Full: 1 + 2*3 - 4/2 + (5%3 << 1) = 1 + 6 - 2 + (2<<1) = 1+6-2+4 = 9? recompute.
// Precedence: << is below +/-, so groups as (1 + 2*3 - 4/2 + 5%3) << 1.
// inner = 1 + 6 - 2 + 2 = 7; 7 << 1 = 14.
int main(void) {
	return 1 + 2 * 3 - 4 / 2 + 5 % 3 << 1;
}
