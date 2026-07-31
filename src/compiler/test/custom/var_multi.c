// expect: 9
// A single declaration with several declarators; b uses an initializer.
int main(void) {
	int a = 4, b = 5, c;
	c = a + b;
	return c;
}
