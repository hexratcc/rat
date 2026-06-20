// expect: 7
// A partially-initialized file-scope struct zero-fills the rest.
struct P { int a; int b; int c; };
struct P g = {7};
int main(void) {
	return g.a + g.b + g.c;
}
