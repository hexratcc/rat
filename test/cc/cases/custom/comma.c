// expect: 7
// Comma evaluates left-to-right and yields the last operand: 3 + 4.
int main(void) {
	return (1, 2, 3 + 4);
}
