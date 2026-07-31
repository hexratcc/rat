// expect: 20
// Dispatch through an array of function pointers.
typedef int (*BinOp)(int, int);
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int main(void) {
	BinOp ops[2];
	ops[0] = add;
	ops[1] = sub;
	return ops[0](10, 3) + ops[1](10, 3);
}
