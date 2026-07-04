// expect: 1
// C99 remainder takes the sign of the dividend: 7 % -3 == 1.
int main(void) {
	return 7 % -3;
}
