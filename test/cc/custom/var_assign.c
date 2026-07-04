// expect: 7
// Plain assignment updates the SSA value seen by later reads.
int main(void) {
	int x = 3;
	x = x + 4;
	return x;
}
