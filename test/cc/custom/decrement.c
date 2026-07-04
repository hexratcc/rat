// expect: 3
// Prefix and postfix -- in a countdown loop.
int main(void) {
	int n = 10;
	int steps = 0;
	while (n-- > 7)
		steps++;
	// n decremented to 6 after the failing test; steps counted 10,9,8 -> 3
	return steps;
}
