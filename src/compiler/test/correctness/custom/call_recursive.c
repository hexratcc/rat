// expect: 120
// Recursion: 5! = 120 (also exercises a forward self-reference).
int fact(int n) {
	if (n <= 1)
		return 1;
	return n * fact(n - 1);
}

int main(void) {
	return fact(5);
}
