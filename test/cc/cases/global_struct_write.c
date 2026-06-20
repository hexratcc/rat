// expect: 17
// A file-scope struct is writable through member assignment.
struct P { int x; int y; };
struct P g = {1, 2};
int main(void) {
	g.x = 10;
	g.y = g.y + 5;
	return g.x + g.y;
}
