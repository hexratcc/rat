// expect: 0

static int a[3][5];
static int c[2][3][4];
struct S {
	int x;
	int m[3][5];
} s;

static int* p1 = &a[2][4];
static int* p2 = a[2];
static int* p3 = a[1] + 2;
static int* p4 = &c[1][2][3];
static int (*p5)[4] = c[1];
static int* p6 = &s.m[1][2];
static int* tab[2] = {&a[0][1], c[1][1]};
struct P {
	int* p;
} sp = {&a[1][3]};

int main(void) {
	static int* l = &a[2][2];
	int bad = 0;
	bad |= (p1 != &a[0][0] + 14) << 0;
	bad |= (p2 != &a[0][0] + 10) << 1;
	bad |= (p3 != &a[0][0] + 7) << 2;
	bad |= (p4 != &c[0][0][0] + 23) << 3;
	bad |= (p5 != &c[1][0]) << 4;
	bad |= (p6 != &s.m[0][0] + 7) << 5;
	bad |= (tab[0] != &a[0][0] + 1) << 6;
	bad |= (tab[1] != &c[0][0][0] + 16) << 7;
	bad |= (sp.p != &a[0][0] + 8) << 8;
	bad |= (l != &a[0][0] + 12) << 9;
	return bad;
}
