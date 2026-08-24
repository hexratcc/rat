// expect: 7
// sizeof is an integer constant expression, so it is valid as a case label.
int main(void) {
	int x = 4;
	switch (x) {
	case sizeof(int):
		return 7;
	default:
		return 0;
	}
}
