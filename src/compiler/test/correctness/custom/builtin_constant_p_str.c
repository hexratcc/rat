// expect: 0
// passes:

int global;

int main(void) {
	if(!__builtin_constant_p("hi"))
		return 1;
	if(!__builtin_constant_p(1))
		return 2;
	if(!__builtin_constant_p((1234 + 45) & ~7))
		return 3;
	if(__builtin_constant_p(global))
		return 4;
	if(__builtin_constant_p(&global))
		return 5;
	return 0;
}
