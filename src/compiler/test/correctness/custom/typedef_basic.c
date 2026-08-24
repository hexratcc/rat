// expect: 9
// A simple typedef alias for a builtin type.
typedef int myint;

int main(void) {
	myint a = 4;
	myint b = 5;
	return a + b;
}
