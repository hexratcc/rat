// expect: 200
// char operands promote to int before addition, so no 8-bit overflow.
int main(void) {
	char a = 100;
	char b = 100;
	return a + b;
}
