// expect: 0
// passes:

long double g;
__attribute__((noinline)) void take(long double x) { g = x; }
void (*volatile fp)(long double) = take;

int main(void) {
	void (*f)(long double) = fp;
	f(1.5L);
	return g == 1.5L ? 0 : 1;
}
