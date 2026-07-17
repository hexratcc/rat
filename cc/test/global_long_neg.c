// expect: -3
// skip-target: windows
// A 64-bit global with a negative constant initializer round-trips correctly.
long big = 5000000004;
int neg = -7;

int main(void) {
	return (int)(big % 100) + neg;
}
