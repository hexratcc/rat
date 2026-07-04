// expect: 8
// Assignment is an expression whose value is the stored value.
int main(void) {
	int a;
	int b;
	b = (a = 8);
	return b;
}
