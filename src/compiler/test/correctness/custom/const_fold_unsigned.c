// expect: 0

enum {
	Half = -1u / 2,
	Less = (-1 < 0u),
	Shifted = -1u >> 30,
	Lazy = 0 && 1 / 0,
};

struct B {
	unsigned x : (-1u >> 30);
};

_Static_assert(-1u / 1073741824 == 3, "unsigned division");

int main(void) {
	if(Half != 2147483647)
		return 1;
	if(Less != 0)
		return 2;
	if(Shifted != 3)
		return 3;
	if(Lazy != 0)
		return 4;
	struct B b = {3};
	if(b.x != 3)
		return 5;
	return 0;
}
