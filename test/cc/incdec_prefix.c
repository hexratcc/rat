// expect: 8
// Prefix ++ yields the updated value: x and y are both 6, so 6+6-4 = 8.
int main(void) {
	int x = 5;
	int y = ++x;
	return x + y - 4;
}
