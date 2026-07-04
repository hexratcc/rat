// expect: 49

// function-like macros with nested invocation and rescanning.
#define SQ(x) ((x) * (x))
#define ADD(a, b) ((a) + (b))

int main(void) {
	return SQ(ADD(3, 4)); // (7)*(7)
}
