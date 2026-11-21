// expect: 0

// _Alignas ahead of a struct definition aligns the object

char pad0;
_Alignas(64) struct { char c; } g1;
char pad1;
_Alignas(64) struct Tagged { char c; } g2;

// the alignment of a member does not leak to what follows the struct
struct Outer {
	struct Inner {
		_Alignas(64) char c;
	}* ptr;
};

int main(void) {
	if(sizeof(struct Outer) != sizeof(void*))
		return 3;
	char pad2;
	_Alignas(64) struct { char c; } l1;
	char pad3;
	_Alignas(64) union { char c; } l2;
	if((unsigned long)&g1 % 64 || (unsigned long)&g2 % 64)
		return 1;
	if((unsigned long)&l1 % 64 || (unsigned long)&l2 % 64)
		return 2;
	return pad0 + pad1 + (&pad2 == &pad3);
}
