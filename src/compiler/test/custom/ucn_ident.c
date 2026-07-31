// expect: 42
// Universal character name in an identifier (C99 6.4.2.1). The same UCN
// spelling refers to the same object.
int main(void) {
	int \u00e9 = 42;
	return \u00e9;
}
