// expect: 0
// passes:

int abs(int);
long labs(long);
long long llabs(long long);

volatile int vi = -7;
volatile long vl = -1234567890123L;
volatile long long vll = -1;

int main(void) {
	if(llabs(vll) != 1)
		return 1;
	if(abs(vi) != 7 || abs(-vi) != 7)
		return 2;
	if(labs(vl) != 1234567890123L)
		return 3;
	if(__builtin_llabs(vll) != 1)
		return 4;
	// the most negative value negates to itself
	vi = -2147483647 - 1;
	if(abs(vi) != -2147483647 - 1)
		return 5;
	return 0;
}

// these must never be reached
int abs(int x) {
	(void)x;
	return 999;
}
long labs(long x) {
	(void)x;
	return 999;
}
long long llabs(long long x) {
	(void)x;
	return 999;
}
