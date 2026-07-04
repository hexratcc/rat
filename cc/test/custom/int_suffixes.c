// expect: 6
// Valid integer/float suffixes (C99 6.4.4.1/6.4.4.2) in all accepted forms.
int main(void) {
	unsigned long a = 1UL;
	unsigned long b = 2ull; // long long collapses to long in this model
	long c = 3LL;
	double d = 1.0f + 2.0L + 0.0; // f and L float suffixes
	(void)b;
	(void)c;
	(void)d;
	return (int)(a + b + c) + 0;
}
