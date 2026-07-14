// expect: 42
// a static function reached only through a function pointer must survive DFE:
// its address is taken, so it cannot be removed even with no direct caller
static int identity(int x) { return x; }
int main(void) {
	int (*fp)(int) = identity;
	return fp(42);
}
