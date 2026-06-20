// expect: 60
// Several declarators in one file-scope declaration, with constant initializers.
int a = 10, b = 20, c = 10 > 20 ? 1 : 30;

int main(void) {
	return a + b + c;
}
