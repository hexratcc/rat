// expect: 160
// Type qualifiers and storage-class specifiers are accepted and ignored.
static const int G = 42;
int main(void) {
	const int x = 10;
	register int y = 5;
	volatile int z = 3;
	unsigned const long w = 100;
	return x + y + z + (int)w + G;
}
