// expect: 20
typedef int (*BinOp)(int, int);
static int add2(int a, int b) { return a + b; }
static int mul2(int a, int b) { return a * b; }
int main(void) {
	BinOp arr[2];
	int i, r = 0;
	arr[0] = add2;
	arr[1] = mul2;
	for (i = 0; i < 4; i++)
		r += arr[i & 1](i, 3);
	return r;
}
