// expect: 25
// Typedef used as a function parameter and return type.
typedef int Int;

Int square(Int n) {
	return n * n;
}

int main(void) {
	return square(5);
}
