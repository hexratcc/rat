// expect: 27
// A struct compound literal used as an lvalue for member access.
struct P { int x; int y; };
int main(void) {
	return (struct P){.y = 7, .x = 2}.x * 10 + (struct P){.y = 7, .x = 2}.y;
}
