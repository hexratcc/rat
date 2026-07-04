// expect: 42
// C99 6.7.8: a brace-enclosed initializer for a union initializes its first
// named member (or a designated member); other members share the storage.
union U {
	int i;
	char c[4];
};

union V {
	short s;
	int x;
};

int main(void) {
	union U a = { 42 };                  // first member
	union U b = { .c = { 1, 2, 3, 4 } }; // designated array member
	union V c = { .x = 100 };            // designated member
	union U z = {};                      // empty braces zero-initialize

	if (a.i != 42)
		return 1;
	if (b.c[0] != 1 || b.c[3] != 4)
		return 2;
	if (c.x != 100)
		return 3;
	if (z.i != 0)
		return 4;
	return 42;
}
