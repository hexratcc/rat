// expect: 7
// Pass a pointer to a function that mutates the pointee.
void addone(int *p) {
	*p = *p + 1;
}

int main(void) {
	int x = 5;
	addone(&x);
	addone(&x);
	return x;
}
