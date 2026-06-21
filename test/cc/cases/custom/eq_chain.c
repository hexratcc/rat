// expect: 1
// Equality is left-associative: (1 == 1) == 1 -> 1 == 1 -> 1.
int main(void) {
	return (1 == 1) == 1;
}
