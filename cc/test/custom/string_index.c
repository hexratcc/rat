// expect: 104
// A string literal assigned to char* and indexed.
int main(void) {
	char *s = "hello";
	return s[0];
}
