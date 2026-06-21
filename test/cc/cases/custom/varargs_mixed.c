// expect: 100

// fetch a mix of types from the variadic list, including a wider 'long' and a
// pointer, and combine them into the result.
long accumulate(int count, ...) {
	va_list ap;
	__builtin_va_start(ap, count);
	long total = 0;
	long scale = __builtin_va_arg(ap, long); // first variadic arg: a long
	int *p = __builtin_va_arg(ap, int *);    // second: a pointer
	total = scale * (*p);                    // 10 * 9 = 90
	for (int i = 2; i < count; ++i)
		total += __builtin_va_arg(ap, int);  // remaining ints
	__builtin_va_end(ap);
	return total;
}

int main(void) {
	int x = 9;
	return (int)accumulate(4, 10L, &x, 4, 6); // 90 + 4 + 6 = 100
}
