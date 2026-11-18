// expect: 22
// The length of an array of structs with brace-elided elements counts
// whole elements, a string fills a char array member.
struct A {
	char n[4];
	int x;
};

struct A g[] = {"ab", 1, "cd", 2};

int main(void) {
	struct A l[] = {"ab", 1, "cd", 2};
	return sizeof g / sizeof g[0] * 10 + sizeof l / sizeof l[0];
}
