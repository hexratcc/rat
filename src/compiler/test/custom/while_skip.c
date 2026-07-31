// expect: 0
// A while loop whose condition is false on entry runs zero times.
int main(void) {
	int s = 0;
	while (s > 10)
		s += 1;
	return s;
}
