// expect: 15
// A global mutated inside a loop via a helper accumulates correctly.
int acc = 0;

void bump(int n) {
	acc += n;
}

int main(void) {
	int i;
	for (i = 1; i <= 5; i++)
		bump(i);
	return acc;
}
