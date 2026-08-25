// expect: 0
// a chain of internal helpers referenced only by each other (and by nothing
// live) must all be eliminated once main no longer calls into them. main here
// never calls them, so DFE removes the whole chain; the program returns 0
static int leaf(int a) { return a + 1; }
static int mid(int a) { return leaf(a) + 1; }
static int top(int a) { return mid(a) + 1; }
int main(void) {
	return 0;
}
