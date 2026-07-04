// expect: 1
// A forward goto skips over the unreachable assignment x = 99.
int main(void) {
	int x = 1;
	goto skip;
	x = 99;
skip:
	return x;
}
