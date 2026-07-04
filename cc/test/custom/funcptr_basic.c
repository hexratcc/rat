// expect: 13
// Call through a local function-pointer variable.
typedef int (*BinOp)(int, int);
int add(int a, int b) { return a + b; }
int main(void) {
	BinOp f = add;
	return f(10, 3);
}
