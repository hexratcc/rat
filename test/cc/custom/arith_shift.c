// expect: -2
// Arithmetic right shift of a negative value sign-extends: -8 >> 2 == -2.
int main(void) {
	return -8 >> 2;
}
