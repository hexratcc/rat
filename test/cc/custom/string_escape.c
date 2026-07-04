// expect: 10
// Escape sequences are decoded inside string literals.
int main(void) {
	char *s = "ab\ncd";
	return s[2];
}
