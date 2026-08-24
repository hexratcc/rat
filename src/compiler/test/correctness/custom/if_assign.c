// expect: 20
// if without else mutates a variable, then falls through to the return.
int main(void) {
	int r = 5;
	if (r == 5)
		r = 20;
	return r;
}
