// expect: 24
// An array declared in code a goto jumps over still gets its size
// from the initializer.
int main(void) {
	goto L;
	int a[] = {[4] = 1};
	char s[] = "abc";
L:
	return sizeof a + sizeof s;
}
