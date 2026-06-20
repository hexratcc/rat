// expect: 22
// Basic function call with parameters and multiple call sites.
int add(int a, int b) {
	return a + b;
}

int main(void) {
	return add(3, 4) + add(10, 5);
}
