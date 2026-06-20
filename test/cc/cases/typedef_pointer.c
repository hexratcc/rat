// expect: 30
// Typedef for a pointer type, used with '->'.
typedef struct Point { int x; int y; } Point;
typedef Point *PointPtr;

int main(void) {
	Point p;
	p.x = 10;
	p.y = 20;
	PointPtr q = &p;
	return q->x + q->y;
}
