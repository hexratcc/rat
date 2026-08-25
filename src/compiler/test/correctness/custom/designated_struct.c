// expect: 250
// Designated struct initializers, out of order, with a zero-filled gap.
struct P { int x; int y; int z; };
int main(void) {
	struct P p = { .y = 5, .x = 2 };
	return p.x * 100 + p.y * 10 + p.z;
}
