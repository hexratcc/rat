// expect: 11
// Postfix ++ yields the old value, but the variable still increments.
int main(void) {
	int x = 5;
	int y = x++;
	return x + y; // 6 + 5
}
