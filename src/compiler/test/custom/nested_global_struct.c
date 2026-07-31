// expect: 10
// A file-scope struct with nested struct members.
struct P { int x; int y; };
struct Line { struct P a; struct P b; };
struct Line g = {{1, 2}, {3, 4}};
int main(void) {
	return g.a.x + g.a.y + g.b.x + g.b.y;
}
