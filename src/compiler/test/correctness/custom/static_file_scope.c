// expect: 42
// file-scope statics have internal linkage: they must still behave as ordinary
// module globals within the TU (initialized, addressable, mutable), while the
// backends give them local/static visibility so identically named statics in
// other TUs cannot collide at link time.
static int counter = 40;
static const int step = 2;
static int table[3] = {1, 2, 3};
static int *ptr = &counter;

int bump(void) {
	*ptr = *ptr + step;
	return counter;
}

int main(void) {
	if (table[0] + table[1] + table[2] != 6)
		return 1;
	return bump();
}
