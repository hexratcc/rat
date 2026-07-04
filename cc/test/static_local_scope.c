// expect: 9
// An inner static shadows an outer one; each has its own storage.
int f(void) {
	static int x = 5;
	{
		static int x = 2;
		return x;
	}
}
int g(void) {
	static int x = 5;
	x = x + 2;
	return x;
}
int main(void) {
	return f() + g();
}
