// expect: 7
// if/else picks the else-branch when the condition is false.
int main(void) {
	int x = 0;
	if (x > 1)
		return 42;
	else
		return 7;
}
