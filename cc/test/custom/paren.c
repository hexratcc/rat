// expect: 21
// Parentheses override precedence: (1+2) * (3+4) == 3 * 7.
int main(void) {
	return ((1 + 2) * (3 + 4));
}
