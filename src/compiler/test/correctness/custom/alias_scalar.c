// expect: 0

int a = 1;
extern int b __attribute__((alias("a")));

static int s = 100;
extern int t __attribute__((alias("s")));

__attribute__((noinline)) static void bump(void) { ++b; }

int main(void) {
	if(b != 1)
		return 1;
	b = 2;
	if(a != 2)
		return 2;
	a = 3;
	if(b != 3)
		return 3;
	bump();
	if(a != 4 || b != 4)
		return 4;

	if(t != 100)
		return 5;
	t = 101;
	if(s != 101)
		return 6;
	return 0;
}
