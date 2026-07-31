// expect: 42
// C99 6.4.4.4p10: a character constant containing more than one character has
// an implementation-defined value. We follow the common host convention and
// pack the bytes big-endian into an int, so 'ab' == 0x6162 and 'abcd' ==
// 0x61626364. A single-character constant keeps the plain character value.
int main(void) {
	int a = 'ab';   // 0x6162     = 24930
	int b = 'A';    //              65
	int c = 'abcd'; // 0x61626364 = 1633837924
	return (a == 24930) && (b == 65) && (c == 1633837924) ? 42 : 0;
}
