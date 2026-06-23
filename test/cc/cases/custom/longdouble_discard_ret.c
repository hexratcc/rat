// expect: 7
long double foo(void) { return 1.5L; }

int main(void) {
	for (int i = 0; i < 20; i++)
		foo(); // result discarded

	long double a = 2.0L, b = 3.0L;
	long double c = a * b + 1.0L; // 7.0
	return (int)c;
}
