// expect: 6
// Nested struct member access through chained '.'.
struct Inner { int a; int b; };
struct Outer { struct Inner in; int c; };

int main(void) {
	struct Outer o;
	o.in.a = 1;
	o.in.b = 2;
	o.c = 3;
	return o.in.a + o.in.b + o.c;
}
