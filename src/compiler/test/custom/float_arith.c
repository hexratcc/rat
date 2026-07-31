// expect: 8
// Basic double arithmetic; (3.5 * 2.0 + 1.0) == 8.0.
int main(void) {
	double a = 3.5;
	double b = 2.0;
	double c = a * b + 1.0;
	return (int)c;
}
