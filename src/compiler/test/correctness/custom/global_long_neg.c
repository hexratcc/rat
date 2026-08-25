// expect: -3
// expect-windows: 1
// A long global initialized past its range round-trips correctly.
long big = 5000000004;
int neg = -7;

int main(void) {
	return (int)(big % 100) + neg;
}
