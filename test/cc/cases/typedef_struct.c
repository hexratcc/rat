// expect: 7
// Typedef of a struct, used without the 'struct' keyword.
typedef struct Point { int x; int y; } Point;

int main(void) {
	Point p;
	p.x = 3;
	p.y = 4;
	return p.x + p.y;
}
