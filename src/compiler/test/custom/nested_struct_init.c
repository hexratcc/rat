// expect: 25
// A struct whose members are themselves structs, initialized with nested braces.
struct P { int x; int y; };
struct Line { struct P a; struct P b; int n; };
int main(void) {
	struct Line l = {{3, 4}, {5, 6}, 7};
	return l.a.x + l.a.y + l.b.x + l.b.y + l.n;
}
