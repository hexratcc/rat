// expect: 10
// C99 6.5.2.5: a compound literal of array-of-struct type creates an unnamed
// object and decays to a pointer to its first element. Each brace group
// initializes one struct element, laid out contiguously.
struct P { int x, y; };
int main(void) {
	struct P *p = (struct P[]){ {1, 2}, {3, 4}, {5, 6} };
	return p[0].x + p[1].y + p[2].x; // 1 + 4 + 5 = 10
}
