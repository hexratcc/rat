// expect: 1
// Floating comparisons yield int 0/1; here 2.5 < 3.0 && 3.0 >= 3.0.
int main(void) {
	double a = 2.5;
	double b = 3.0;
	int lt = a < b;       // 1
	int ge = b >= 3.0;    // 1
	int eq = (a == b);    // 0
	return lt && ge && !eq;
}
