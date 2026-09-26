// expect: 0
// output:
//| 2420
// passes:

long f(long n, long a) {
	long x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11;
	{
		long v[n];
		__asm__ volatile("" ::"r"(v) : "memory");
		x1 = a * 3 + 1;
		x2 = a * 4 + 2;
		x3 = a * 5 + 3;
		x4 = a * 6 + 4;
		x5 = a * 7 + 5;
		x6 = a * 8 + 6;
		x7 = a * 9 + 7;
		x8 = a * 10 + 8;
		x9 = a * 11 + 9;
		x10 = a * 12 + 10;
		x11 = a * 13 + 11;
	}
	return x1 * 1 + x2 * 2 + x3 * 3 + x4 * 4 + x5 * 5 + x6 * 6 + x7 * 7 + x8 * 8 + x9 * 9 + x10 * 10 +
				 x11 * 11;
}

int main(void) {
	__builtin_printf("%ld\n", f(7, 3));
	return 0;
}
