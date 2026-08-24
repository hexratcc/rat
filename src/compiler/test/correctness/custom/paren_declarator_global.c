// expect: 69
static const char *(azHelp[]) = {"ab", "cde"};
int (gx) = 5;
int *(gy[3]);
int (*ga[4])(void);
static int (*(*gf)(int))[3];
typedef int myint;
myint (gm) = 9;
struct T {
	int (m);
	int (n)[3];
	int (*fp)(void);
};

static int one(void) { return 1; }
static int two(void) { return 2; }

static int arr3[3] = {7, 8, 9};
static int (*retArr(int k))[3] {
	(void)k;
	return &arr3;
}

static int slen(const char *s) {
	int n = 0;
	while(s[n])
		n++;
	return n;
}

int main(void) {
	struct T t;
	int (lx) = 3;
	int *(ly[2]);
	int r = 0;
	ga[0] = one;
	ga[1] = two;
	gf = retArr;
	t.m = 4;
	t.n[0] = 10;
	t.n[1] = 11;
	t.n[2] = 12;
	t.fp = two;
	ly[0] = &lx;
	ly[1] = &gx;
	gy[0] = &gm;
	if((int)sizeof(t.n) != 3 * (int)sizeof(int))
		return 98;
	if((int)sizeof(struct T) != (int)(4 * sizeof(int) + sizeof(int (*)(void))))
		return 99;
	r += slen(azHelp[0]) + slen(azHelp[1]); // 2 + 3
	r += gx + gm + lx;                      // 5 + 9 + 3
	r += ga[0]() + ga[1]();                 // 1 + 2
	r += (*gf(0))[2];                       // 9
	r += t.m + t.n[2] + t.fp();             // 4 + 12 + 2
	r += *ly[0] + *ly[1] + *gy[0];          // 3 + 5 + 9
	return r;                               // 5 + 17 + 3 + 9 + 18 + 17 == 69
}
