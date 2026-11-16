// expect: 0
// offsetof steps through both dimensions of a 2D member.
struct S {
	int x;
	int m[2][3];
};

int main(void) {
	if(__builtin_offsetof(struct S, m[1][2]) != sizeof(int) + 5 * sizeof(int))
		return 1;
	return __builtin_offsetof(struct S, m[1]) != sizeof(int) + 3 * sizeof(int);
}
