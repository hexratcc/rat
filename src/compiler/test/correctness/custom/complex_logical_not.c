// expect: 0

// !z of a complex z is true only when both parts are zero
int main(void) {
	double _Complex one = 1.0;
	double _Complex zero = 0.0;
	return !one != 0 || !zero != 1;
}
