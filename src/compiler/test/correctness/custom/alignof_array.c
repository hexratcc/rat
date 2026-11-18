// expect: 0
struct S {
	char m[10];
};

int main(void) {
	char a[10];
	struct S s;
	// an array aligns as its element
	if(_Alignof(a) != 1)
		return 1;
	if(_Alignof(s.m) != 1)
		return 2;
	return 0;
}
