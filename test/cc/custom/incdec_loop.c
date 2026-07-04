// expect: 45
// for loop using postfix ++ as the post-expression.
int main(void) {
	int s = 0;
	for (int i = 0; i < 10; i++)
		s += i;
	return s;
}
