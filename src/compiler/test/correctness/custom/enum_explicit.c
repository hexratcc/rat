// expect: 7
// Explicit enumerator values, including expressions referencing earlier ones.
enum Flags { A = 1, B = 2, C = 4, ALL = A | B | C };

int main(void) {
	return ALL;
}
