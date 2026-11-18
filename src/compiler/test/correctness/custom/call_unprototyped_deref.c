// expect: 7
// A call through the dereferenced name of an unprototyped function takes
// any arguments.
int f() { return 7; }

int main(void) {
	return (*f)(1);
}
