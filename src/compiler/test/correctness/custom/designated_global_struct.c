// expect: 109
// Designated initializers on a file-scope struct.
struct P { int a; int b; int c; };
struct P g = { .c = 9, .a = 1 };
int main(void) {
	return g.a * 100 + g.b * 10 + g.c;
}
