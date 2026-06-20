// expect: 3
// ?: is right-associative: 0 ? 1 : (0 ? 2 : 3).
int main(void) {
	return 0 ? 1 : 0 ? 2 : 3;
}
