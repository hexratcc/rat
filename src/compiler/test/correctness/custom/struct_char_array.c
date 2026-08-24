// expect: 82
// A char array member plus a trailing scalar; size reflects alignment padding.
struct S { char name[8]; int v; };

int main(void) {
	struct S s;
	s.name[0] = 'A';
	s.v = 5;
	return s.name[0] + s.v + sizeof(struct S);
}
