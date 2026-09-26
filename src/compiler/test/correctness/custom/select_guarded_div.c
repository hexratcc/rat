// expect: 0

volatile int zero = 0;
volatile int minusOne = -1;
volatile int intMin = -2147483647 - 1;

__attribute__((noinline)) int sdiv(int v) { return v && 7 / v; }

__attribute__((noinline)) int srem(int v) {
	int r = 0;
	if(v)
		r = (7 % v + 1) << 1;
	return r;
}

__attribute__((noinline)) int udiv(unsigned v) { return v ? 7u / v != 0 : 0; }

__attribute__((noinline)) int urem(unsigned v) { return v && 7u % v; }

__attribute__((noinline)) int overflow(int a, int b) { return b != -1 && a / b; }

int main(void) {
	if(sdiv(zero))
		return 1;
	if(srem(zero))
		return 2;
	if(udiv(zero))
		return 3;
	if(urem(zero))
		return 4;
	if(overflow(intMin, minusOne))
		return 5;
	return 0;
}
