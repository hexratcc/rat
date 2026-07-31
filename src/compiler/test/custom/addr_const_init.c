// expect: 166

// Static initializers containing address constants: a table of function
// pointers, a pointer to a string literal, and a pointer to another global.
// These cannot be encoded as raw bytes; they become relocations.
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }

typedef int (*op)(int, int);

op ops[2] = {add, sub};
const char *msg = "hello";
int g = 42;
int *gp = &g;

int main(void) {
	int r = ops[0](10, 3) + ops[1](10, 3); // 13 + 7 == 20
	r += msg[0];                           // 'h' == 104
	r += *gp;                              // 42
	return r;                              // 20 + 104 + 42 == 166
}
