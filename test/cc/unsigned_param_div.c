// expect: 2000000000
// Unsigned parameters force udiv: 4000000000u / 2u == 2000000000.
unsigned int half(unsigned int a, unsigned int b) {
	return a / b;
}

int main(void) {
	return (int)half(4000000000u, 2u);
}
