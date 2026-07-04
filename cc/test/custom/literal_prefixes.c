// expect: 108

// C99 encoding prefixes on char and string literals: L/u/U on chars,
// L/u/U/u8 on strings. The prefix is consumed and the (narrow) value used.
int main(void) {
	int a = L'A';       // 65
	int b = u'\a';      // 7
	char *s = u8"hello";
	int c = s[1];       // 'e' == 101 ; 7 + 101 == 108 ; with 65 - 65 == 108
	return a - L'A' + b + c;
}
