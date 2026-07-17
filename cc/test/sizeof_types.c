// expect: 15
// skip-target: windows
// Byte sizes of the basic integer types: 1 + 2 + 4 + 8.
int main(void) {
	return sizeof(char) + sizeof(short) + sizeof(int) + sizeof(long);
}
