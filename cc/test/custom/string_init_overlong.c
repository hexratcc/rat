// expect: 0
// C99 6.7.8p14: a string literal initializing a sized char array copies only
// the characters that fit. A trailing NUL that would not fit is dropped, and an
// over-long literal has its excess characters ignored.
void abort(void);

// global: exactly enough room for the chars but not the NUL
char g3[3] = "abc";
// global: literal longer than the array -> first 2 chars only
char g2[2] = "wxyz";
// global 2-D: an element's literal is over-long (the classic PR86714 shape)
const char gm[2][3] = { "1234", "xy" };

int main(void) {
	// local: NUL dropped
	char l3[3] = "abc";
	// local: over-long, excess ignored
	char l2[2] = "wxyz";

	if (g3[0] != 'a' || g3[1] != 'b' || g3[2] != 'c')
		abort();
	if (g2[0] != 'w' || g2[1] != 'x')
		abort();
	if (gm[0][0] != '1' || gm[0][1] != '2' || gm[0][2] != '3')
		abort();
	if (gm[1][0] != 'x' || gm[1][1] != 'y' || gm[1][2] != 0)
		abort();
	if (l3[0] != 'a' || l3[1] != 'b' || l3[2] != 'c')
		abort();
	if (l2[0] != 'w' || l2[1] != 'x')
		abort();
	return 0;
}
