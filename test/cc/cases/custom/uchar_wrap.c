// expect: 2
// unsigned char wraps modulo 256: 258 - 256 == 2.
int main(void) {
	unsigned char c = 258;
	return c;
}
