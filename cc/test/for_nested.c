// expect: 9
// Nested for loops: 3 x 3 increments of an accumulator.
int main(void) {
	int c = 0;
	for (int i = 0; i < 3; i += 1)
		for (int j = 0; j < 3; j += 1)
			c += 1;
	return c;
}
