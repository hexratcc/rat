// expect: 25
// Pointer to struct with '->' member access, passed to a function.
struct Point { int x; int y; };

int sumpt(struct Point *p) {
	return p->x + p->y;
}

int main(void) {
	struct Point p;
	p.x = 10;
	p.y = 20;
	struct Point *q = &p;
	q->x = 5;
	return sumpt(q);
}
