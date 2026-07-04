// expect: 1
// && binds tighter than ||: 0 || (1 && 1) == 0 || 1 == 1.
int main(void) {
	return 0 || 1 && 1;
}
