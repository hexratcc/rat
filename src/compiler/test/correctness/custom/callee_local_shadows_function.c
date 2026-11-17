// expect: 0
int f(void) { return 1; }
int two(void) { return 2; }

int call(int (*f)(void)) { return f(); }

int callAddr(int (*f)(void)) {
	int (**pf)(void) = &f;
	return (*pf)();
}

int main(void) {
	if(call(two) != 2)
		return 1;
	if(callAddr(two) != 2)
		return 2;
	return 0;
}
