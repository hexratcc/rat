// expect: 1
// sizeof yields size_t (unsigned); comparing it against a signed value still
// works for small constants. sizeof(int) == 4, so (sizeof(int) > 0) is 1.
int main(void) {
	return sizeof(int) > 0;
}
