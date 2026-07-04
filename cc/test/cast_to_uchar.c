// expect: 255
// (unsigned char)(-1) keeps the low 8 bits: 255.
int main(void) {
	int x = -1;
	return (unsigned char)x;
}
