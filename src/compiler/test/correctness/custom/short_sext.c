// expect: -7
// A signed short sign-extends when promoted to int.
int main(void) {
	short s = -7;
	return s;
}
