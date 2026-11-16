// expect: 0
// A block scope extern array keeps its declared size with no file scope declaration.
// sizeof does not reference eb, so no definition is needed.
int main(void) {
	extern int eb[5];
	return sizeof eb != 5 * sizeof(int);
}
