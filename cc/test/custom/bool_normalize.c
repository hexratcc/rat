// expect: 1
// Converting a nonzero value to _Bool normalizes it to 1.
int main(void) {
	_Bool b = 5;
	return b;
}
