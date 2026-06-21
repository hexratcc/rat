// expect: 4
// -(-5) == 5, ~0 == -1, sum == 4.
int main(void) {
	return -(-5) + ~0;
}
