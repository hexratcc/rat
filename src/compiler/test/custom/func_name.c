// expect: 102

// __func__ is a predefined static string holding the enclosing function name.
int foo(void) {
	return __func__[0]; // 'f' == 102
}

int main(void) {
	return foo();
}
