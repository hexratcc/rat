// expect: 6

// __VA_ARGS__ forwarded through a macro into a variadic function.
#define CALL(...) sum(__VA_ARGS__)

int sum(int count, ...) {
	va_list ap;
	__builtin_va_start(ap, count);
	int total = 0;
	for (int i = 0; i < count; ++i)
		total += __builtin_va_arg(ap, int);
	__builtin_va_end(ap);
	return total;
}

int main(void) {
	return CALL(3, 1, 2, 3);
}
