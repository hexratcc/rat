// expect: 0
// A block-scope function declaration hides an outer local of the same name.
int f(void) {
	return 1;
}
int two(void) {
	return 2;
}
int g(void) {
	return 3;
}
int main(void) {
	int (*f)(void) = two;
	char g = 7;
	{
		int f(void);
		extern int g(void);
		int (*q)(void) = &f;
		if(f() != 1 || q() != 1 || sizeof(g()) != sizeof(int) || g() != 3)
			return 1;
	}
	return f() != 2 || g != 7;
}
