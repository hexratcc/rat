// expect: 12
// A brace initializer that supplies every member of a struct.
struct P { int x; int y; };
int main(void) {
	struct P p = {3, 4};
	return p.x * p.y;
}
