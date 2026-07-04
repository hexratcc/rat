// expect: 1
// 0u - 1 == 0xFFFFFFFF; a logical right shift by 31 yields 1.
int main(void) {
	unsigned int x = 0;
	x = x - 1;
	return (int)(x >> 31);
}
