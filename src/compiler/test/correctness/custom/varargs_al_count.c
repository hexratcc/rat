// expect: 0
// passes:

double dsum(int n, ...) {
	__builtin_va_list ap;
	__builtin_va_start(ap, n);
	double t = 0;
	for(int i = 0; i < n; ++i)
		t += __builtin_va_arg(ap, double);
	__builtin_va_end(ap);
	return t;
}

// eight vector arguments: AL must say 8, and the ninth spills to the stack
double dmany(int n, double a, double b, double c, double d, ...) {
	__builtin_va_list ap;
	__builtin_va_start(ap, d);
	double t = a + b + c + d;
	for(int i = 4; i < n; ++i)
		t += __builtin_va_arg(ap, double);
	__builtin_va_end(ap);
	return t;
}

long isum(int n, ...) {
	__builtin_va_list ap;
	__builtin_va_start(ap, n);
	long t = 0;
	for(int i = 0; i < n; ++i)
		t += __builtin_va_arg(ap, long);
	__builtin_va_end(ap);
	return t;
}

// mixed classes: AL counts only the vector arguments
long mixed(int n, ...) {
	__builtin_va_list ap;
	__builtin_va_start(ap, n);
	long t = 0;
	for(int i = 0; i < n; ++i) {
		t += __builtin_va_arg(ap, int);
		t += (long)__builtin_va_arg(ap, double);
	}
	__builtin_va_end(ap);
	return t;
}

static int argCount(void) { return 3; }

int main(void) {
	if(dsum(3, 1.5, 2.5, 3.0) != 7.0)
		return 1;
	if(dmany(9, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0) != 45.0)
		return 2;

	// ten integers: six in registers, four on the stack
	if(isum(10, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L) != 55L)
		return 3;
	if(mixed(2, 1, 10.0, 2, 20.0) != 33L)
		return 4;

	// argument 0 comes out of a call of its own
	if(dsum(argCount(), 1.0, 2.0, 4.0) != 7.0)
		return 5;

	// through a variadic function pointer
	double (*pd)(int, ...) = dsum;
	long (*pi)(int, ...) = isum;
	if(pd(2, 8.0, 9.0) != 17.0)
		return 6;
	if(pi(3, 100L, 200L, 300L) != 600L)
		return 7;
	return 0;
}
