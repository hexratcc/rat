// expect: 12
// expect-windows: 8
// sizeof of an expression uses its type without evaluating it.
int main(void) {
	long a = 0;
	char b = 0;
	return sizeof a + sizeof(b + b);
}
