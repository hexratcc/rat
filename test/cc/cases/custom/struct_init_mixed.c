// expect: 1131
// Brace initialization of a mixed-type struct honors member offsets/padding.
struct M { char a; int b; char c; };
int main(void) {
	struct M m = {65, 1000, 66};
	return m.a + m.b + m.c;
}
