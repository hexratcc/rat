// expect: -4
// C99 truncates toward zero: (-7)/2 == -3, (-7)%2 == -1, sum == -4.
int main(void) {
	return (-7) / 2 + (-7) % 2;
}
