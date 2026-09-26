// expect: 0

volatile int zero = 0;
volatile int minusOne = -1;
volatile int intMin = -2147483647 - 1;

__attribute__((noinline)) int sdiv(int v) {
	int i, r = 0;
	for(i = 0; i < 3; i++)
		if(v == 0 || 7 / v)
			r++;
	return r;
}

__attribute__((noinline)) int srem(int v) {
	int i, r = 0;
	for(i = 0; i < 3; i++)
		if(v == 0 || 7 % v)
			r++;
	return r;
}

__attribute__((noinline)) int udiv(unsigned v) {
	int i, r = 0;
	for(i = 0; i < 3; i++)
		if(v == 0 || 7u / v)
			r++;
	return r;
}

__attribute__((noinline)) int urem(unsigned v) {
	int i, r = 0;
	for(i = 0; i < 3; i++)
		if(v == 0 || 7u % v)
			r++;
	return r;
}

__attribute__((noinline)) int overflow(int a, int b) {
	int i, r = 0;
	for(i = 0; i < 3; i++)
		if(b == -1 || (a / b) * 2 + a % b > 1) // chain of uses
			r++;
	return r;
}

int main(void) {
	if(sdiv(zero) != 3)
		return 1;
	if(srem(zero) != 3)
		return 2;
	if(udiv(zero) != 3)
		return 3;
	if(urem(zero) != 3)
		return 4;
	if(overflow(intMin, minusOne) != 3)
		return 5;
	return 0;
}
