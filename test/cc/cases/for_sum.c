// expect: 45
// Classic for loop summing 0..9.
int main(void) {
	int s = 0;
	for (int i = 0; i < 10; i += 1)
		s += i;
	return s;
}
