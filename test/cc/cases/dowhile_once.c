// expect: 1
// do/while always runs the body at least once, even when the cond is false.
int main(void) {
	int n = 0;
	do {
		n += 1;
	} while (n < 0);
	return n;
}
