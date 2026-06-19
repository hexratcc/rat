// expect: 42

// token pasting with `##` forms a new identifier.
#define CAT(a, b) a##b
#define VALUE 42

int main(void) {
	int xy = VALUE;
	return CAT(x, y);
}
