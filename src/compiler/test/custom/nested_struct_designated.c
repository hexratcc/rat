// expect: 1209
// Designated initializers mixing nested sub-objects, out of order.
struct P { int x; int y; };
struct Line { struct P a; struct P b; };
int main(void) {
	struct Line l = { .b = {.y = 9}, .a = {1, 2} };
	return l.a.x * 1000 + l.a.y * 100 + l.b.x * 10 + l.b.y;
}
