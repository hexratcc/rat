// expect: 1
// identical string literals are interned to one module global by content, so
// repeated uses of the same literal must compare equal by address, while
// distinct contents keep distinct storage. (C leaves literal identity
// unspecified; ratcc guarantees per-module content interning.)
int main(void) {
	const char *a = "interned";
	const char *b = "interned";
	const char *c = "different";
	if (a != b)
		return 0;
	if (a == c)
		return 0;
	if (a[0] != 'i' || c[0] != 'd')
		return 0;
	return 1;
}
