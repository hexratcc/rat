// expect: 0

int main(void) {
	long long x = -5000000000LL;
	int c = 1;
	long long r = c ? __builtin_llabs(x) : 0;
	if(r != 5000000000LL)
		return 1;
	if(sizeof(__builtin_llabs(x)) != 8 || sizeof(__builtin_labs(x)) != 8)
		return 2;
	if(sizeof(__builtin_bswap64(1)) != 8 || sizeof(__builtin_bswap16(1)) != 2)
		return 3;
	return 0;
}
