// expect: 15
// expect-windows: 11
// Byte sizes of the basic integer types: 1 + 2 + 4 + sizeof(long).
int main(void) {
	return sizeof(char) + sizeof(short) + sizeof(int) + sizeof(long);
}
