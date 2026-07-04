// expect: 7
// A partial brace initializer zero-fills the trailing members.
struct P { int x; int y; int z; };
int main(void) {
	struct P p = {7};
	return p.x + p.y + p.z;
}
