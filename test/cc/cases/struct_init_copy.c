// expect: 14
// Copy-initializing one struct from another of the same type.
struct P { int x; int y; };
int main(void) {
	struct P a = {5, 9};
	struct P b = a;
	return b.x + b.y;
}
