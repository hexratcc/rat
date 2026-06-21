// expect: 6
// A static local has static storage duration: it persists across calls and is
// initialized only once.
int next(void) {
	static int counter = 0;
	counter = counter + 1;
	return counter;
}
int main(void) {
	return next() + next() + next();
}
