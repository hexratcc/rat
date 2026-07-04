// expect: 64
// A struct with an array member, initialized with a nested brace list.
struct R { int v[3]; int n; };
int main(void) {
	struct R r = {{10, 20, 30}, 4};
	return r.v[0] + r.v[1] + r.v[2] + r.n;
}
