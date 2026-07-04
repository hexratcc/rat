// expect: 42
// Simple if with a true condition returns from the then-branch.
int main(void) {
	int x = 3;
	if (x > 1)
		return 42;
	return 7;
}
