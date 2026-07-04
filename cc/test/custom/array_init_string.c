// expect: 294
// A char array initialized from a string literal (length inferred, NUL-terminated).
// 'a'+'b'+'c'+'\0' == 294.
int main(void) {
	char s[] = "abc";
	return s[0] + s[1] + s[2] + s[3];
}
