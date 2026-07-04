// expect: 30
// Floating ternary: select between two double values without control flow.
int main(void) {
	double a = 1.0;
	double b = 2.0;
	double x = a < b ? 10.5 : 99.0;   // 10.5
	double y = a > b ? 99.0 : 20.25;  // 20.25
	return (int)x + (int)y;            // 10 + 20 = 30
}
