// expect: 42
// Case labels accept integer constant expressions: 2 + 4 == 6.
int main(void) {
	int x = 6;
	switch (x) {
	case 2 + 4:
		return 42;
	default:
		return 0;
	}
}
