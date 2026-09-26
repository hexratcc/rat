// expect: 0

volatile int zero = 0;
volatile int one = 1;
volatile int minusOne = -1;
volatile int intMin = -2147483647 - 1;

__attribute__((noinline)) int sdiv(int c, int v) {
	int r = 0;
	if(c) {
		if(v && 7 / v > 1)
			r++;
	} else {
		if(v && 7 / v > 2)
			r += 2;
	}
	return r;
}

__attribute__((noinline)) int srem(int c, int v) {
	int r = 0;
	if(c) {
		if(v && 7 % v > 1)
			r++;
	} else {
		if(v && 7 % v > 2)
			r += 2;
	}
	return r;
}

__attribute__((noinline)) int udiv(int c, unsigned v) {
	int r = 0;
	if(c) {
		if(v && 7u / v > 1)
			r++;
	} else {
		if(v && 7u / v > 2)
			r += 2;
	}
	return r;
}

__attribute__((noinline)) int urem(int c, unsigned v) {
	int r = 0;
	if(c) {
		if(v && 7u % v > 1)
			r++;
	} else {
		if(v && 7u % v > 2)
			r += 2;
	}
	return r;
}

__attribute__((noinline)) int overflow(int c, int a, int b) {
	int r = 0;
	if(c) {
		if(b != -1 && a / b > 1)
			r++;
	} else {
		if(b != -1 && a / b > 2)
			r += 2;
	}
	return r;
}

int main(void) {
	if(sdiv(one, zero) + sdiv(zero, zero))
		return 1;
	if(srem(one, zero) + srem(zero, zero))
		return 2;
	if(udiv(one, zero) + udiv(zero, zero))
		return 3;
	if(urem(one, zero) + urem(zero, zero))
		return 4;
	if(overflow(one, intMin, minusOne) + overflow(zero, intMin, minusOne))
		return 5;
	return 0;
}
