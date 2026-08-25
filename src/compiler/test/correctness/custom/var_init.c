// expect: 30
// Initialized locals fold through to the return.
int main(void) {
	int a = 10;
	int b = 20;
	return a + b;
}
