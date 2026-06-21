// expect: 11
// A function prototype lets two functions be mutually recursive.
int is_even(int n);
int is_odd(int n) {
	if (n == 0)
		return 0;
	return is_even(n - 1);
}
int is_even(int n) {
	if (n == 0)
		return 1;
	return is_odd(n - 1);
}
int main(void) {
	return is_even(10) * 10 + is_odd(7);
}
