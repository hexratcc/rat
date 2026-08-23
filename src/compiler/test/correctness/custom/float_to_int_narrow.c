// expect: 0
volatile double big = 18446744073709551615.0;

int main(void) {
	if((int)big != -2147483648)
		return 1;
	if((int)(double)(unsigned long)(long)-1 != -2147483648)
		return 2;
	if((int)1.5e0 != 1) // in-range conversions are unchanged
		return 3;
	if((unsigned int)4294967295.0 != 4294967295u)
		return 4;
	return 0;
}
