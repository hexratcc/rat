// expect: 214
// A file-scope struct with a char-array member initialized by a string.
struct R { char s[4]; int n; };
struct R g = {"hi", 5};
int main(void) {
	return g.s[0] + g.s[1] + g.n;
}
