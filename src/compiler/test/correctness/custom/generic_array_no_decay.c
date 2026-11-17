// expect: 48
// A _Generic selecting an array keeps the array type under sizeof.
int g[12];
int main(void) {
	return sizeof(_Generic(0, int: g));
}
