// expect: 4
// Universal character name in a string literal (C99 6.4.3): \u00e9 encodes to
// the two UTF-8 bytes 0xC3 0xA9, so "ab\u00e9" is 4 bytes long ('a','b',+2).
int strlen_(const char *s) {
	int n = 0;
	while (s[n])
		++n;
	return n;
}
int main(void) { return strlen_("ab\u00e9"); }
