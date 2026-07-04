// expect: 25
// continue skips even numbers; sum of odds 1+3+5+7+9.
int main(void) {
	int s = 0;
	for (int i = 0; i < 10; i += 1) {
		if (i % 2 == 0)
			continue;
		s += i;
	}
	return s;
}
