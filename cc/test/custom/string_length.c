// expect: 11
// Walk a string literal to its NUL terminator; adjacent literals concatenate.
int slen(char *s) {
	int n = 0;
	while (s[n])
		n++;
	return n;
}

int main(void) {
	char *s = "hello" " " "world";
	return slen(s);
}
