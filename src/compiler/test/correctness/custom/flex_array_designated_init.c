// expect: 0
// Designators size a statically initialized flexible array member.
struct F {
	int n;
	int f[];
};

struct F s = {1, {[3] = 5}};
struct F t = {1, .f[2] = 7};

int main(void) {
	if(s.n != 1 || s.f[0] != 0 || s.f[3] != 5)
		return 1;
	if(t.n != 1 || t.f[1] != 0 || t.f[2] != 7)
		return 2;
	return 0;
}
