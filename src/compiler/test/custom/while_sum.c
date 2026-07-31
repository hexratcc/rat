// expect: 10
// Accumulate 0+1+2+3+4 with a while loop.
int main(void) {
	int s = 0;
	int i = 0;
	while (i < 5) {
		s += i;
		i += 1;
	}
	return s;
}
