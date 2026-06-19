// expect: 30
// continue in a while loop must still make progress via the pre-increment.
// i is incremented first, so i takes 1..10; even values 2+4+6+8+10 = 30.
int main(void) {
	int s = 0;
	int i = 0;
	while (i < 10) {
		i += 1;
		if (i % 2 == 1)
			continue;
		s += i;
	}
	return s;
}
