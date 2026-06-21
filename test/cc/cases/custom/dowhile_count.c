// expect: 6
// do/while counts 1+2+3 with a post-tested condition.
int main(void) {
	int s = 0;
	int i = 1;
	do {
		s += i;
		i += 1;
	} while (i <= 3);
	return s;
}
