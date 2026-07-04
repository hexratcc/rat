// expect: 133
// Struct assignment performs a value copy.
struct Point { int x; int y; };

int main(void) {
	struct Point a;
	a.x = 11;
	a.y = 22;
	struct Point b;
	b = a;
	b.x = 100;
	return b.x + b.y + a.x;
}
