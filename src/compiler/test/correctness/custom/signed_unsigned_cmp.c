// expect: 1
// -1 converts to a huge unsigned value, so (-1 > 0u) is true.
int main(void) {
	int a = -1;
	unsigned int b = 0;
	return a > b;
}
